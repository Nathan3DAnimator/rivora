// Materials (material.h, notas-dev/materiais.md): the graph, the .rmat / .rmi files, the compiler (static part baked,
// dynamic part per pixel), the GPU textures and the dynamic instances.
#include "material.h"
#include "tex_settings.h" // (2026-11-23: material_gpu.inc reads them)

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


#include "../shaders/mat_ops.h"
#include "io_json.h"
#include "platform.h"
#include "platform_files.h"
#include "render_gpu.h"
#include "vkctx.h"
#include "async.h"
#include "i18n.h"
#include "udim.h" // (Q18) UDIM textures: material_udim.inc

static const uint32_t s_bake[] =
#include "material_bake.comp.inc"
    ;
static const uint32_t s_ahist[] = // (a Masked base color's alpha histogram per mip: the coverage kept, bake_variant)
#include "alpha_hist.comp.inc"
    ;
static const uint32_t s_bc7[] = // (the baked textures and the image files compressed: tex_to_bc7)
#include "bc7.comp.inc"
    ;
static const uint32_t s_mip[] = // (2026-11-23: the baked textures' mips by a Kaiser filter in linear space: tex_mips_hq)
#include "mip_kaiser.comp.inc"
    ;

#define MAT_TEX_MAX 4096      // textures of the bindless set (sources and baked; streaming swaps in new ones)
#define MAT_GPU_SLOTS 4096    // GpuMaterial entries (materials, instances, dynamic instances)
#define MAT_PROG_WORDS (1u << 20)
#define MAT_PARAM_VEC4 (1u << 16)
#define MAT_MAX_ASSETS 1024
#define MAT_MAX_DYNAMIC 4096

typedef struct GpuMaterial { // shaders/mat_vm.glsl GpuMaterial
    uint32_t tex_base, tex_nrm, tex_emis, flags;
    uint32_t prog_off, prog_len, param_off, nparams;
    uint32_t anim[4];  // an animated material: its frames (0: still), its period (float bits), its frames' table (param index); (Q17) z of a still one: the displacement's magnitude and center (two halfs, MATF_TESS)
    float avg[3], uv_scale;
} GpuMaterial;
_Static_assert(sizeof(GpuMaterial) == 64, "mat_vm.glsl GpuMaterial");
// the Principled BSDF's extra lobes per slot, a table after the GpuMaterials in the same buffer (mat_common.glsl GpuMatExt)
typedef struct GpuMatExt { float e[13][4]; } GpuMatExt; // (mat_common.glsl e0-e12; e12: Q27.8's flakes)
_Static_assert(MAT_GPU_SLOTS == 4096, "mat_common.glsl MAT_EXT_BASE");
// (Q11) the Toon shading model's block per slot, a table after the GpuMatExt one (toon.glsl GpuToon; toon.c toon_pack)
typedef struct GpuToon { float t[12][4]; } GpuToon;


typedef struct BakePush { // material_bake.comp
    VkDeviceAddress prog, params;
    uint32_t off, len, w, h;
    uint32_t tex_base, tex_nrm, tex_emis, flags;
    uint32_t slot_tex[MAT_MAX_SLOTS];
    uint32_t org[2]; // the region's origin (texture streaming bakes in tiles)
    VkDeviceAddress udim; // (Q18) the parameters buffer: the UDIM images' tables (mat_vm.glsl mat_sample_udim)
    float uv_off[2];      // (Q18) a UDIM tile's origin in the UV (its bake covers that unit square)
} BakePush;

typedef struct MatTex {
    VkImage image;
    VkDeviceMemory memory;
    VkImageView view, store_view; // all mips (sampled), mip 0 (the bake writes it)
    uint32_t w, h, mips;
    VkFormat format;
    char source[MAT_PATH]; // an image file ("": baked)
    int refs;
    uint64_t bytes; // its memory (the streaming budget counts it)
    int block;      // its memory's block (tex_mem_alloc; -1: an allocation of its own)
    VkDeviceSize offset;
    bool clamp;     // (Q18) read with the clamping sampler (a UDIM tile's: no bleed from its opposite edge)
} MatTex;

// A baked variant of a material: its textures with one set of parameter values (the material's own, a Material
// Instance's, an actor's dynamic instance's); a variant without parameters to differ shares its material's textures
typedef struct Variant {
    uint32_t slot, param_off;
    uint32_t tex[3];   // base, normal/roughness/metallic, emission (0: none / shared)
    bool own, dirty;   // own textures (else the material's); rebake before the next frame
    float avg[3];
    float avg_emis[3]; // (2026-11-23) the baked emission's mean (the texture's units: x100 nits)
    // (Q18) UDIM (material_udim.inc): a tile variant bakes the unit square at uv_off; an owner (a material's, an instance's,
    // an actor's variant of a UDIM material) has its tiles' table in the parameters (ud_table) and its tiles in the pool
    float uv_off[2];
    uint32_t ud_table;
    uint16_t ud_first, ud_n;
    bool ud_tile;
} Variant;

typedef struct Dyn { Entity e; Material* m; Variant v; } Dyn;

// the compiled form kept per material: its bake program and its parameters
typedef struct Compiled {
    uint32_t bake_off, bake_len, res;
    uint32_t flags;    // MATF_EMISSIVE
    char pnames[MAT_MAX_PARAMS][MAT_NAME]; // the parameters the program reads, in MOP_PARAM order
    float pdefault[MAT_MAX_PARAMS][4];
    int nparams;
    int nbaked;        // (2026-11-16) the parameters the bake reads (the first ones); the rest only the Wet Surface's (read at run time: no bake)
    Variant v;         // the material's own
    bool valid;
    bool animated;     // a flipbook (its variants are not streamed)
    float world_tile;  // World Aligned: cm a tile (MATF_WORLD_UV)
    uint32_t tess_mc;  // (Q17) MATF_TESS: the magnitude and center as two halfs (GpuMaterial.anim[2])
    float ext[13][4];  // the Principled BSDF's extra lobes as constants (MATF_EXT: principled_ext); e6: M5's (write_slot); e7: M7's
    uint32_t xflags;   // MATX_ (mat_common.glsl): the lobes, the Color Attribute's tint
    MatToon toon;      // (Q11) the material's toon and outline values (its slots' GpuToon; an instance's own over them)
    uint32_t detail_hash, detail_slot; // M5's detail normal: its material, the GpuMaterial slot written (mat_update follows it)
    uint32_t layer_hash[3], layer_slot[3]; // (B11) the painted layers' materials and their slots (followed as the detail's)
    bool layer_tried;
    bool detail_tried; // (its material looked for once: not in the Content folder, not again every frame)
    // animated (a Time node): frame 0 is v, the rest here; their textures' table in the parameters
    Variant frames[MAT_ANIM_MAX];
    uint32_t nframes, table_off;
    float period;
    // the flipbook's frames baked a few per editor frame (anim_continue): the next one, how many, the hidden time's index;
    // until they all are, the material shows frame 0 still
    uint32_t anim_next, anim_total;
    int time_param;
} Compiled;

static struct {
    VkCtx* vk;
    char root[MAT_PATH];
    bool ready, scanned;
    Material* mats[MAT_MAX_ASSETS];
    Compiled comp[MAT_MAX_ASSETS];
    uint32_t n;
    MatTex tex[MAT_TEX_MAX];
    uint32_t ntex;
    VkDescriptorSetLayout set_layout;
    VkDescriptorPool pool;
    VkDescriptorSet set;
    VkPipelineLayout layout;
    VkPipeline bake, bc7;
    VkPipeline mip;  // (2026-11-23) mip_kaiser.comp: the baked textures' mips (tex_mips_hq)
    GpuBuf mip_buf;  // its output, copied into each level (grown to the largest mip 1 met; reused, barriers between)
    VkSampler sampler, sampler_clamp; // (Q18: the clamping one for the UDIM tiles)
    GpuBuf materials, programs, params;
    uint32_t nslots, prog_used, param_used;
    Dyn dyn[MAT_MAX_DYNAMIC];
    uint32_t ndyn;
    MatStats stats;
    bool any_dirty;
    bool resend; // (Q22) a slot an instance points to changed (a dynamic one made or freed): mat_update says changed
    bool any_detail; // a material with a detail normal (M5: mat_update follows their slots)
} M;

// ============================================================================================
// The graph
// ============================================================================================

void mat_node_init(MatNode* n, int type) {
    memset(n, 0, sizeof *n);
    n->type = type;
    const MatNodeDef* d = mat_node_def(type);
    if (!d) return;
    for (int i = 0; i < d->nin; i++) memcpy(n->in[i], d->in[i].def, sizeof n->in[i]);
    for (int p = 0; p < d->nprops; p++) n->prop[p] = d->props[p].def;
    if (type == NODE_RGB) n->value[0] = n->value[1] = n->value[2] = 0.5f, n->value[3] = 1.0f;
    if (type == NODE_VALUE) n->value[0] = 0.5f;
    if (type == NODE_COLOR_RAMP) { // black to white, Blender's new ramp
        n->ramp_count = 2;
        float a[5] = { 0, 0, 0, 0, 1 }, b[5] = { 1, 1, 1, 1, 1 };
        memcpy(n->ramp[0], a, sizeof a);
        memcpy(n->ramp[1], b, sizeof b);
    }
}

// (2026-11-23) A texture's map by its name: its stem split in words (by _ - . space and by case: "WoodBaseColor" is wood
// base color), the resolution words left off the end (2k, 4096, 8K), then its last one or two words against the
// endings (the longest first). key: the words before them run together, lower case
int mat_map_slot(const char* file, char* key, size_t cap) {
    static const struct { const char* end; int slot; } ENDS[] = {
        { "basecolor", MAT_MAP_BASE }, { "basecolour", MAT_MAP_BASE }, { "albedo", MAT_MAP_BASE }, { "diffuse", MAT_MAP_BASE },
        { "diff", MAT_MAP_BASE }, { "color", MAT_MAP_BASE }, { "colour", MAT_MAP_BASE }, { "col", MAT_MAP_BASE }, { "bc", MAT_MAP_BASE },
        { "d", MAT_MAP_BASE }, { "normaldx", MAT_MAP_NORMAL_DX }, { "nordx", MAT_MAP_NORMAL_DX }, { "normalgl", MAT_MAP_NORMAL },
        { "norgl", MAT_MAP_NORMAL }, { "normal", MAT_MAP_NORMAL }, { "normals", MAT_MAP_NORMAL }, { "normalmap", MAT_MAP_NORMAL },
        { "nrm", MAT_MAP_NORMAL }, { "nor", MAT_MAP_NORMAL }, { "norm", MAT_MAP_NORMAL }, { "n", MAT_MAP_NORMAL },
        { "roughness", MAT_MAP_ROUGH }, { "rough", MAT_MAP_ROUGH }, { "rgh", MAT_MAP_ROUGH }, { "r", MAT_MAP_ROUGH },
        { "glossiness", MAT_MAP_GLOSS }, { "gloss", MAT_MAP_GLOSS }, { "metallic", MAT_MAP_METAL }, { "metalness", MAT_MAP_METAL },
        { "metal", MAT_MAP_METAL }, { "mtl", MAT_MAP_METAL }, { "m", MAT_MAP_METAL }, { "orm", MAT_MAP_ORM }, { "arm", MAT_MAP_ORM },
        { "occlusionroughnessmetallic", MAT_MAP_ORM }, { "emissive", MAT_MAP_EMIT }, { "emission", MAT_MAP_EMIT }, { "emit", MAT_MAP_EMIT },
        { "glow", MAT_MAP_EMIT }, { "opacity", MAT_MAP_ALPHA }, { "alpha", MAT_MAP_ALPHA }, { "mask", MAT_MAP_ALPHA },
        { "ambientocclusion", MAT_MAP_AO }, { "occlusion", MAT_MAP_AO }, { "ao", MAT_MAP_AO }, { "height", MAT_MAP_HEIGHT },
        { "displacement", MAT_MAP_HEIGHT }, { "disp", MAT_MAP_HEIGHT }, { "bump", MAT_MAP_HEIGHT },
    };
    const char* bn = file + strlen(file);
    while (bn > file && bn[-1] != '/' && bn[-1] != '\\') bn--;
    char words[16][40];
    int nw = 0;
    size_t wl = 0;
    const char* dot = strrchr(bn, '.');
    const char* end = dot ? dot : bn + strlen(bn);
    for (const char* c = bn; c <= end && nw < 16; c++) {
        bool alnum = c < end && ((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9'));
        bool upper = c < end && *c >= 'A' && *c <= 'Z';
        bool lower_before = c > bn && c[-1] >= 'a' && c[-1] <= 'z';
        if ((!alnum || (upper && lower_before)) && wl) words[nw][wl] = 0, nw++, wl = 0; // (a separator, or "aB")
        if (alnum && nw < 16 && wl + 1 < sizeof words[0]) words[nw][wl++] = (char)(upper ? *c - 'A' + 'a' : *c);
    }
    while (nw > 1) { // the resolution off the end (2k, 4096, 8k, 1024px)
        const char* w = words[nw - 1];
        const char* q = w;
        while (*q >= '0' && *q <= '9') q++;
        if (q == w || (*q && strcmp(q, "k") && strcmp(q, "px"))) break;
        nw--;
    }
    int slot = MAT_MAP_NONE, used = 0;
    for (int take = 2; take >= 1 && !slot; take--) { // the last two words run together ("base color", "nor gl"), then the last
        if (nw - take < 1) continue;
        char tail[80] = "";
        for (int k = nw - take; k < nw; k++) strncat(tail, words[k], sizeof tail - strlen(tail) - 1);
        for (size_t e = 0; e < sizeof ENDS / sizeof ENDS[0] && !slot; e++)
            if (!strcmp(tail, ENDS[e].end) && (take == 2 || strlen(ENDS[e].end) > 1 || nw >= 2)) slot = ENDS[e].slot, used = take;
    }
    if (!slot && nw == 1) // one word: a long ending at its end ("woodroughness")
        for (size_t e = 0; e < sizeof ENDS / sizeof ENDS[0] && !slot; e++) {
            size_t el = strlen(ENDS[e].end), l = strlen(words[0]);
            if (el >= 5 && l > el && !strcmp(words[0] + l - el, ENDS[e].end)) slot = ENDS[e].slot, words[0][l - el] = 0;
        }
    if (key && cap) {
        key[0] = 0;
        for (int k = 0; k < nw - used; k++) strncat(key, words[k], cap - strlen(key) - 1);
    }
    return slot;
}

int mat_graph_add(MatGraph* g, int type, float x, float y) {
    if (g->node_count >= MAT_MAX_NODES || !mat_node_def(type)) return -1;
    MatNode* n = &g->nodes[g->node_count];
    mat_node_init(n, type);
    n->x = x;
    n->y = y;
    return g->node_count++;
}

void mat_graph_remove(MatGraph* g, int idx) {
    if (idx < 0 || idx >= g->node_count) return;
    int w = 0;
    for (int i = 0; i < g->link_count; i++) {
        MatLink l = g->links[i];
        if (l.from == idx || l.to == idx) continue;
        if (l.from > idx) l.from--;
        if (l.to > idx) l.to--;
        g->links[w++] = l;
    }
    g->link_count = w;
    memmove(&g->nodes[idx], &g->nodes[idx + 1], sizeof(MatNode) * (size_t)(g->node_count - idx - 1));
    g->node_count--;
}

int mat_graph_input_link(const MatGraph* g, int to, int to_sock) {
    for (int i = 0; i < g->link_count; i++)
        if (g->links[i].to == to && g->links[i].to_sock == to_sock) return i;
    return -1;
}

void mat_graph_unlink_input(MatGraph* g, int to, int to_sock) {
    int i = mat_graph_input_link(g, to, to_sock);
    if (i < 0) return;
    g->links[i] = g->links[--g->link_count];
}

static bool depends(const MatGraph* g, int node, int on) { // node reads (directly or not) from on
    if (node == on) return true;
    for (int i = 0; i < g->link_count; i++)
        if (g->links[i].to == node && depends(g, g->links[i].from, on)) return true;
    return false;
}

bool mat_graph_link(MatGraph* g, int from, int from_sock, int to, int to_sock) {
    if (from < 0 || to < 0 || from >= g->node_count || to >= g->node_count || from == to) return false;
    const MatNodeDef *fd = mat_node_def(g->nodes[from].type), *td = mat_node_def(g->nodes[to].type);
    if (!fd || !td || from_sock < 0 || from_sock >= fd->nout || to_sock < 0 || to_sock >= td->nin) return false;
    bool fs = fd->out[from_sock].type == MSOCK_SHADER, ts = td->in[to_sock].type == MSOCK_SHADER;
    bool knot = g->nodes[from].type == MAT_NODE_REROUTE || g->nodes[to].type == MAT_NODE_REROUTE; // (a knot carries shaders too)
    if (fs != ts && !knot) return false; // a shader goes only into a shader socket
    if (depends(g, from, to)) return false; // a cycle
    mat_graph_unlink_input(g, to, to_sock);
    if (g->link_count >= MAT_MAX_LINKS) return false;
    g->links[g->link_count++] = (MatLink){ (int16_t)from, (int16_t)from_sock, (int16_t)to, (int16_t)to_sock };
    return true;
}

void mat_graph_default(MatGraph* g) {
    memset(g, 0, sizeof *g);
    int b = mat_graph_add(g, NODE_BSDF_PRINCIPLED, -260.0f, -40.0f);
    int o = mat_graph_add(g, NODE_OUTPUT_MATERIAL, 60.0f, 0.0f);
    mat_graph_link(g, b, 0, o, 0);
    g->view[2] = 1.0f;
}

void mat_ramp_eval(const MatNode* n, float f, float rgba[4]) {
    int c = n->ramp_count > 0 ? n->ramp_count : 1;
    if (f <= n->ramp[0][0] || c == 1) {
        memcpy(rgba, &n->ramp[0][1], 16);
        return;
    }
    for (int i = 1; i < c; i++)
        if (f < n->ramp[i][0]) {
            float t = (f - n->ramp[i - 1][0]) / fmaxf(n->ramp[i][0] - n->ramp[i - 1][0], 1e-6f);
            if (n->prop[0] == RAMP_CONSTANT) t = 0.0f;
            else if (n->prop[0] == RAMP_EASE) t = t * t * (3.0f - 2.0f * t);
            for (int k = 0; k < 4; k++) rgba[k] = n->ramp[i - 1][1 + k] * (1.0f - t) + n->ramp[i][1 + k] * t;
            return;
        }
    memcpy(rgba, &n->ramp[c - 1][1], 16);
}

// ============================================================================================
// Files: .rmat (a graph, by Blender's idnames and socket names: the Tlender's materials read the same) and .rmi
// ============================================================================================

static char* read_all(const char* path, size_t* len) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* b = n > 0 ? malloc((size_t)n + 1) : NULL; // a load, not a frame
    if (b && fread(b, 1, (size_t)n, f) != (size_t)n) free(b), b = NULL;
    fclose(f);
    if (b) b[n] = 0, *len = (size_t)n;
    return b;
}

static int find_socket(const MatSockDef* s, int n, const char* name) {
    for (int i = 0; i < n; i++)
        if (!strcmp(s[i].name, name)) return i;
    return -1;
}

static void read_params(Material* m, const JsonValue* arr) {
    m->nparams = 0;
    for (size_t i = 0; i < json_len(arr) && m->nparams < MAT_MAX_PARAMS; i++) {
        const JsonValue* p = json_at(arr, i);
        MatParam* q = &m->params[m->nparams++];
        snprintf(q->name, sizeof q->name, "%s", json_get_str(p, "name", ""));
        q->kind = (uint8_t)json_get_int(p, "kind", 0);
        q->value[3] = 1.0f;
        json_get_floats(p, "value", q->value, 4);
    }
}

static bool parse(Material* m, const char* text, size_t len) {
    JsonDoc doc;
    char err[160];
    if (!json_parse(text, len, &doc, err, sizeof err)) {
        snprintf(m->error, sizeof m->error, "%s", err);
        return false;
    }
    const JsonValue* r = &doc.root;
    m->resolution = json_get_int(r, "resolution", 1024);
    m->world_grid = json_get_bool(r, "world_grid", false);
    const char* bm = json_get_str(r, "blend_mode", "Opaque");
    m->blend_mode = !strcmp(bm, "Masked") ? MAT_BLEND_MASKED : !strcmp(bm, "Translucent") ? MAT_BLEND_TRANSLUCENT : MAT_BLEND_OPAQUE;
    m->two_sided = json_get_bool(r, "two_sided", false);
    m->refraction = (float)json_get_num(r, "refraction", 1.0); // (2026-11-16: glass bends what is behind by default)
    m->refr_thickness = (float)json_get_num(r, "refraction_thickness", 5.0);
    m->dispersion = (float)json_get_num(r, "dispersion", 0.0);
    m->transmission_mode = !strcmp(json_get_str(r, "transmission_mode", "Refractive"), "Thin") ? MAT_TRANS_THIN : MAT_TRANS_REFRACTIVE; // (Q4)
    m->absorption[0] = (float)json_get_num(r, "absorption_r", 1.0), m->absorption[1] = (float)json_get_num(r, "absorption_g", 1.0);
    m->absorption[2] = (float)json_get_num(r, "absorption_b", 1.0), m->absorption_dist = (float)json_get_num(r, "absorption_distance", 10.0);
    m->colored_shadow = json_get_bool(r, "colored_shadow", true);
    { const char* smn = json_get_str(r, "shading_model", "Default Lit"); // (Q11: Toon)
      m->shading_model = !strcmp(smn, "Eye") ? MAT_SHADING_EYE : !strcmp(smn, "Toon") ? MAT_SHADING_TOON : MAT_SHADING_DEFAULT; }
    m->domain = !strcmp(json_get_str(r, "domain", "Surface"), "Light Function") ? MAT_DOMAIN_LIGHT_FUNCTION : MAT_DOMAIN_SURFACE; // (Q4)
    m->eye_iris_radius = (float)json_get_num(r, "iris_radius", 0.25);
    m->eye_iris_depth = (float)json_get_num(r, "iris_depth", 0.25);
    m->eye_glow = (float)json_get_num(r, "eye_glow", 0.0); // (Q13)
    m->eye_glow_color[0] = (float)json_get_num(r, "eye_glow_r", 0.85), m->eye_glow_color[1] = (float)json_get_num(r, "eye_glow_g", 1.0);
    m->eye_glow_color[2] = (float)json_get_num(r, "eye_glow_b", 0.7);
    m->emissive_light = (float)json_get_num(r, "emissive_light", 1.0); // (2026-11-23)
    m->emissive_shadows = json_get_bool(r, "emissive_shadows", false);
    m->retro = (float)json_get_num(r, "retro", 0.0); // (Q27.8)
    m->retro_color[0] = (float)json_get_num(r, "retro_r", 1.0), m->retro_color[1] = (float)json_get_num(r, "retro_g", 1.0);
    m->retro_color[2] = (float)json_get_num(r, "retro_b", 1.0);
    m->flakes = (float)json_get_num(r, "flakes", 0.0), m->flake_size = (float)json_get_num(r, "flake_size", 0.04);
    m->flake_spread = (float)json_get_num(r, "flake_spread", 0.35);
    m->world_aligned = json_get_bool(r, "world_aligned", false);
    m->world_tile = (float)json_get_num(r, "world_tile", 200.0);
    m->tessellation = json_get_bool(r, "tessellation", false); // (Q17)
    m->disp_magnitude = (float)json_get_num(r, "displacement_magnitude", 5.0);
    m->disp_center = (float)json_get_num(r, "displacement_center", 0.5);
    snprintf(m->detail_path, sizeof m->detail_path, "%s", json_get_str(r, "detail_normal", "")); // (M5)
    m->detail_tiling = (float)json_get_num(r, "detail_tiling", 8.0);
    m->detail_strength = (float)json_get_num(r, "detail_strength", 1.0);
    { // (B11) the painted layers
        const JsonValue* ly = json_get(r, "layers");
        for (int k = 0; k < 3; k++) {
            const JsonValue* o = json_at(ly, (size_t)k);
            snprintf(m->layer_path[k], sizeof m->layer_path[k], "%s", o ? json_get_str(o, "material", "") : "");
            m->layer_tile[k] = o ? (float)json_get_num(o, "tile", 400.0) : 400.0f;
        }
    }
    m->opacity_clip = (float)json_get_num(r, "opacity_mask_clip", MAT_OPACITY_CLIP_DEFAULT);
    m->anim_frames = json_get_int(r, "anim_frames", 16);
    m->anim_res = json_get_int(r, "anim_res", 512);
    read_params(m, json_get(r, "parameters"));
    toon_read(&m->toon, r); // (Q11) an instance overrides them when its file has them ("toon_own")
    m->toon_own = m->instance && json_get_bool(r, "toon_own", false);
    if (m->instance) {
        snprintf(m->parent_path, sizeof m->parent_path, "%s", json_get_str(r, "parent", ""));
        json_free(&doc);
        return true;
    }
    MatGraph* g = m->graph;
    memset(g, 0, sizeof *g);
    g->view[2] = 1.0f;
    json_get_floats(r, "view", g->view, 3);
    const JsonValue* nodes = json_get(r, "nodes");
    for (size_t i = 0; i < json_len(nodes) && g->node_count < MAT_MAX_NODES; i++) {
        const JsonValue* jn = json_at(nodes, i);
        const MatNodeDef* d = mat_node_def_by_idname(json_get_str(jn, "idname", ""));
        MatNode* n = &g->nodes[g->node_count++];
        mat_node_init(n, d ? d->type : NODE_VALUE); // an unknown node: a Value (its links fail, the graph still opens)
        n->x = (float)json_get_num(jn, "x", 0);
        n->y = (float)json_get_num(jn, "y", 0);
        int pr[SVM_PROPS];
        for (int p = 0; p < SVM_PROPS; p++) pr[p] = n->prop[p];
        const JsonValue* props = json_get(jn, "props");
        for (size_t p = 0; p < json_len(props) && p < SVM_PROPS; p++) pr[p] = json_int(json_at(props, p), pr[p]);
        memcpy(n->prop, pr, sizeof pr);
        const JsonValue* ins = json_get(jn, "inputs");
        if (d && ins && ins->type == JSON_OBJECT)
            for (uint32_t k = 0; k < ins->count; k++) {
                int s = find_socket(d->in, d->nin, ins->members[k].key);
                if (s < 0) continue;
                for (size_t c = 0; c < 4 && c < json_len(&ins->members[k].value); c++)
                    n->in[s][c] = (float)json_num(json_at(&ins->members[k].value, c), n->in[s][c]);
            }
        json_get_floats(jn, "value", n->value, 4);
        const JsonValue* ramp = json_get(jn, "ramp");
        if (json_len(ramp)) {
            n->ramp_count = 0;
            for (size_t s = 0; s < json_len(ramp) && s < RAMP_MAX_STOPS; s++) {
                const JsonValue* st = json_at(ramp, s);
                for (int c = 0; c < 5; c++) n->ramp[n->ramp_count][c] = (float)json_num(json_at(st, (size_t)c), 0.0);
                n->ramp_count++;
            }
        }
        snprintf(n->image, sizeof n->image, "%s", json_get_str(jn, "image", ""));
        snprintf(n->label, sizeof n->label, "%s", json_get_str(jn, "label", ""));
        snprintf(n->param, sizeof n->param, "%s", json_get_str(jn, "param", ""));
    }
    const JsonValue* links = json_get(r, "links");
    for (size_t i = 0; i < json_len(links); i++) {
        const JsonValue* l = json_at(links, i);
        int from = json_get_int(l, "from", -1), to = json_get_int(l, "to", -1);
        if (from < 0 || to < 0 || from >= g->node_count || to >= g->node_count) continue;
        const MatNodeDef *fd = mat_node_def(g->nodes[from].type), *td = mat_node_def(g->nodes[to].type);
        if (!fd || !td) continue;
        int fs = find_socket(fd->out, fd->nout, json_get_str(l, "from_socket", ""));
        int ts = find_socket(td->in, td->nin, json_get_str(l, "to_socket", ""));
        if (fs >= 0 && ts >= 0) mat_graph_link(g, from, fs, to, ts);
    }
    json_free(&doc);
    return true;
}

static void write_params(JsonWriter* w, const Material* m) {
    jw_key(w, "parameters");
    jw_arr_begin(w);
    for (int i = 0; i < m->nparams; i++) {
        jw_obj_begin(w);
        jw_kstr(w, "name", m->params[i].name);
        jw_key(w, "kind");
        jw_int(w, m->params[i].kind);
        jw_key(w, "value");
        jw_arr_begin(w);
        for (int c = 0; c < 4; c++) jw_float(w, m->params[i].value[c]);
        jw_arr_end(w);
        jw_obj_end(w);
    }
    jw_arr_end(w);
}

bool mat_save(Material* m) {
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kstr(&w, "format", m->instance ? "RealEngine Material Instance" : "RealEngine Material");
    jw_key(&w, "version");
    jw_int(&w, 1);
    jw_key(&w, "resolution");
    jw_int(&w, m->resolution);
    jw_key(&w, "anim_frames");
    jw_int(&w, m->anim_frames);
    jw_key(&w, "anim_res");
    jw_int(&w, m->anim_res);
    if (m->world_grid) {
        jw_key(&w, "world_grid");
        jw_bool(&w, true);
    }
    if (m->blend_mode == MAT_BLEND_MASKED) {
        jw_kstr(&w, "blend_mode", "Masked");
        jw_kfloat(&w, "opacity_mask_clip", m->opacity_clip);
    }
    if (m->blend_mode == MAT_BLEND_TRANSLUCENT) jw_kstr(&w, "blend_mode", "Translucent");
    if (m->two_sided) jw_kbool(&w, "two_sided", true);
    if (m->blend_mode == MAT_BLEND_TRANSLUCENT || m->transmissive)
        jw_kfloat(&w, "refraction", m->refraction), jw_kfloat(&w, "refraction_thickness", m->refr_thickness), jw_kfloat(&w, "dispersion", m->dispersion);
    if (m->transmission_mode == MAT_TRANS_THIN) jw_kstr(&w, "transmission_mode", "Thin"); // (Q4)
    if (m->absorption[0] < 1.0f || m->absorption[1] < 1.0f || m->absorption[2] < 1.0f)
        jw_kfloat(&w, "absorption_r", m->absorption[0]), jw_kfloat(&w, "absorption_g", m->absorption[1]), jw_kfloat(&w, "absorption_b", m->absorption[2]),
            jw_kfloat(&w, "absorption_distance", m->absorption_dist);
    if (!m->colored_shadow) jw_kbool(&w, "colored_shadow", false);
    if (m->shading_model == MAT_SHADING_EYE)
        jw_kstr(&w, "shading_model", "Eye"), jw_kfloat(&w, "iris_radius", m->eye_iris_radius), jw_kfloat(&w, "iris_depth", m->eye_iris_depth);
    if (m->shading_model == MAT_SHADING_TOON) jw_kstr(&w, "shading_model", "Toon"); // (Q11)
    if (!m->instance || m->toon_own) toon_write(&w, &m->toon, m->instance || m->shading_model == MAT_SHADING_TOON);
    if (m->instance && m->toon_own) jw_kbool(&w, "toon_own", true);
    if (m->shading_model == MAT_SHADING_EYE && m->eye_glow > 0.0f) // (Q13)
        jw_kfloat(&w, "eye_glow", m->eye_glow), jw_kfloat(&w, "eye_glow_r", m->eye_glow_color[0]), jw_kfloat(&w, "eye_glow_g", m->eye_glow_color[1]),
            jw_kfloat(&w, "eye_glow_b", m->eye_glow_color[2]);
    if (m->emissive_light != 1.0f) jw_kfloat(&w, "emissive_light", m->emissive_light); // (2026-11-23)
    if (m->emissive_shadows) jw_kbool(&w, "emissive_shadows", true);
    if (m->retro > 0.0f) // (Q27.8)
        jw_kfloat(&w, "retro", m->retro), jw_kfloat(&w, "retro_r", m->retro_color[0]), jw_kfloat(&w, "retro_g", m->retro_color[1]),
            jw_kfloat(&w, "retro_b", m->retro_color[2]);
    if (m->flakes > 0.0f) jw_kfloat(&w, "flakes", m->flakes), jw_kfloat(&w, "flake_size", m->flake_size), jw_kfloat(&w, "flake_spread", m->flake_spread);
    if (m->domain == MAT_DOMAIN_LIGHT_FUNCTION) jw_kstr(&w, "domain", "Light Function"); // (Q4)
    if (m->world_aligned) jw_kbool(&w, "world_aligned", true), jw_kfloat(&w, "world_tile", m->world_tile);
    if (m->tessellation) // (Q17)
        jw_kbool(&w, "tessellation", true), jw_kfloat(&w, "displacement_magnitude", m->disp_magnitude), jw_kfloat(&w, "displacement_center", m->disp_center);
    if (m->detail_path[0])
        jw_kstr(&w, "detail_normal", m->detail_path), jw_kfloat(&w, "detail_tiling", m->detail_tiling), jw_kfloat(&w, "detail_strength", m->detail_strength);
    if (m->layer_path[0][0] || m->layer_path[1][0] || m->layer_path[2][0]) { // (B11)
        jw_key(&w, "layers");
        jw_arr_begin(&w);
        for (int k = 0; k < 3; k++) {
            jw_obj_begin(&w);
            jw_kstr(&w, "material", m->layer_path[k]), jw_kfloat(&w, "tile", m->layer_tile[k]);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
    }
    write_params(&w, m);
    if (m->instance) {
        jw_kstr(&w, "parent", m->parent_path);
    } else {
        const MatGraph* g = m->graph;
        jw_key(&w, "view");
        jw_arr_begin(&w);
        for (int i = 0; i < 3; i++) jw_float(&w, g->view[i]);
        jw_arr_end(&w);
        jw_key(&w, "nodes");
        jw_arr_begin(&w);
        for (int i = 0; i < g->node_count; i++) {
            const MatNode* n = &g->nodes[i];
            const MatNodeDef* d = mat_node_def(n->type);
            jw_obj_begin(&w);
            jw_kstr(&w, "idname", d ? d->idname : "");
            jw_kfloat(&w, "x", n->x);
            jw_kfloat(&w, "y", n->y);
            jw_key(&w, "props");
            jw_arr_begin(&w);
            for (int p = 0; p < SVM_PROPS; p++) jw_int(&w, n->prop[p]);
            jw_arr_end(&w);
            jw_key(&w, "inputs");
            jw_obj_begin(&w);
            for (int s = 0; d && s < d->nin; s++) {
                if (d->in[s].type == MSOCK_SHADER) continue;
                jw_key(&w, d->in[s].name);
                jw_arr_begin(&w);
                for (int c = 0; c < 4; c++) jw_float(&w, n->in[s][c]);
                jw_arr_end(&w);
            }
            jw_obj_end(&w);
            jw_key(&w, "value");
            jw_arr_begin(&w);
            for (int c = 0; c < 4; c++) jw_float(&w, n->value[c]);
            jw_arr_end(&w);
            if (n->type == NODE_COLOR_RAMP) {
                jw_key(&w, "ramp");
                jw_arr_begin(&w);
                for (int s = 0; s < n->ramp_count; s++) {
                    jw_arr_begin(&w);
                    for (int c = 0; c < 5; c++) jw_float(&w, n->ramp[s][c]);
                    jw_arr_end(&w);
                }
                jw_arr_end(&w);
            }
            if (n->image[0]) jw_kstr(&w, "image", n->image);
            if (n->label[0]) jw_kstr(&w, "label", n->label);
            if (n->param[0]) jw_kstr(&w, "param", n->param);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
        jw_key(&w, "links");
        jw_arr_begin(&w);
        for (int i = 0; i < g->link_count; i++) {
            const MatLink* l = &g->links[i];
            const MatNodeDef *fd = mat_node_def(g->nodes[l->from].type), *td = mat_node_def(g->nodes[l->to].type);
            if (!fd || !td) continue;
            jw_obj_begin(&w);
            jw_key(&w, "from");
            jw_int(&w, l->from);
            jw_kstr(&w, "from_socket", fd->out[l->from_sock].name);
            jw_key(&w, "to");
            jw_int(&w, l->to);
            jw_kstr(&w, "to_socket", td->in[l->to_sock].name);
            jw_obj_end(&w);
        }
        jw_arr_end(&w);
    }
    jw_obj_end(&w);
    bool ok = !w.fail;
    char tmp[MAT_PATH + 8];
    snprintf(tmp, sizeof tmp, "%s.tmp", m->path);
    FILE* f = ok ? re_fopen(tmp, "wb") : NULL;
    ok = f && fwrite(w.buf, 1, w.len, f) == w.len;
    if (f) ok = (fclose(f) == 0) && ok;
    jw_free(&w);
    return ok && re_replace_file(tmp, m->path);
}

// ============================================================================================
// The registry: every material of the Content folder, found by the hash of its virtual path
// ============================================================================================

static uint32_t vpath_hash(const char* vpath) { // the same as Blueprints' (bp_vpath_hash)
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) {
        char ch = *c == '\\' ? '/' : *c;
        h = (h ^ (uint8_t)tolower((unsigned char)ch)) * 16777619u;
    }
    return h ? h : 1u;
}

bool mat_vpath(const Material* m, char* out, size_t cap) {
    size_t rl = strlen(M.root);
    if (_strnicmp(m->path, M.root, rl)) return false;
    snprintf(out, cap, "/Content%s", m->path + rl);
    for (char* c = out; *c; c++)
        if (*c == '\\') *c = '/';
    return true;
}

static bool ends_with(const char* s, const char* ext) {
    size_t a = strlen(s), b = strlen(ext);
    return a >= b && !_stricmp(s + a - b, ext);
}

Material* mat_load(const char* path_in) {
    char path[MAT_PATH]; // one spelling of a path (backslashes): a file loads once
    snprintf(path, sizeof path, "%s", path_in);
    for (char* c = path; *c; c++)
        if (*c == '/') *c = '\\';
    for (uint32_t i = 0; i < M.n; i++)
        if (!_stricmp(M.mats[i]->path, path)) return M.mats[i];
    if (M.n >= MAT_MAX_ASSETS) return NULL;
    size_t len;
    char* text = read_all(path, &len);
    if (!text) return NULL;
    Material* m = calloc(1, sizeof *m);
    if (!m) return free(text), NULL;
    snprintf(m->path, sizeof m->path, "%s", path);
    m->instance = ends_with(path, ".rmi");
    if (!m->instance) m->graph = calloc(1, sizeof(MatGraph));
    const char* base = strrchr(path, '\\');
    const char* b2 = strrchr(path, '/');
    if (b2 > base) base = b2;
    snprintf(m->name, sizeof m->name, "%s", base ? base + 1 : path);
    char* dot = strrchr(m->name, '.');
    if (dot) *dot = 0;
    bool ok = (m->instance || m->graph) && parse(m, text, len);
    free(text);
    if (!ok) {
        free(m->graph);
        free(m);
        return NULL;
    }
    char vp[MAT_PATH + 16];
    m->hash = mat_vpath(m, vp, sizeof vp) ? vpath_hash(vp) : vpath_hash(path);
    m->dirty = true;
    M.any_dirty = true;
    M.mats[M.n++] = m;
    if (m->instance && m->parent_path[0]) {
        char pp[MAT_PATH * 2];
        if (!strncmp(m->parent_path, "/Content", 8)) snprintf(pp, sizeof pp, "%s%s", M.root, m->parent_path + 8);
        else snprintf(pp, sizeof pp, "%s", m->parent_path);
        m->parent = mat_load(pp);
    }
    return m;
}

static void scan(const char* dir);
static void scan_entry(void* user, const ReDirEntry* e) {
    const char* dir = user;
    char p[MAT_PATH];
    snprintf(p, sizeof p, "%s\\%s", dir, e->name);
    if (e->is_dir) scan(p);
    else if (ends_with(e->name, ".rmat") || ends_with(e->name, ".rmi")) mat_load(p);
}
static void scan(const char* dir) { re_dir_walk(dir, scan_entry, (void*)dir); }

const char* mat_root(void) { return M.root; }

bool mat_cuts(const Material* m) {
    const Material* p = m && m->instance && m->parent ? m->parent : m;
    return p && p->blend_mode != MAT_BLEND_TRANSLUCENT && (p->blend_mode == MAT_BLEND_MASKED || p->two_sided);
}
bool mat_ext(const Material* m) {
    const Material* p = m && m->instance && m->parent ? m->parent : m;
    return p && (p->ext_lobes || p->wet || mat_toon(p) || p->interior || p->vcol == MAT_VCOL_TINT || p->detail_path[0] || p->parallax || p->layer_path[0][0] || p->layer_path[1][0] ||
                 p->layer_path[2][0]); // (+ M5's vertex color and detail normal, M7's parallax, B11's layers)
}

bool mat_toon(const Material* m) { // (Q11)
    const Material* p = m && m->instance && m->parent ? m->parent : m;
    return p && p->shading_model == MAT_SHADING_TOON && p->blend_mode != MAT_BLEND_TRANSLUCENT;
}
const MatToon* mat_toon_values(const Material* m) {
    if (!m) return NULL;
    if (m->instance && !m->toon_own && m->parent) return &m->parent->toon;
    return &m->toon;
}

bool mat_sss(const Material* m) {
    const Material* p = m && m->instance && m->parent ? m->parent : m;
    return p && p->subsurface && p->blend_mode == MAT_BLEND_OPAQUE && !p->transmissive && !p->two_sided;
}

bool mat_tess(const Material* m) { // (Q17)
    const Material* p = m && m->instance && m->parent ? m->parent : m;
    return p && p->tess_active;
}

bool mat_glass_shadow(const Material* m) { // (Q4)
    const Material* p = m && m->instance ? m->parent : m;
    return p && p->transmissive && p->transmission_mode == MAT_TRANS_REFRACTIVE && p->colored_shadow;
}
bool mat_refract(const Material* m) {
    const Material* p = m && m->instance && m->parent ? m->parent : m;
    return p && (p->blend_mode == MAT_BLEND_TRANSLUCENT || p->transmissive) && p->refraction > 0.0f;
}
bool mat_translucent(const Material* m) {
    const Material* p = m && m->instance && m->parent ? m->parent : m;
    return p && (p->blend_mode == MAT_BLEND_TRANSLUCENT || p->transmissive); // (a Transmission Weight: glass through the OIT pass)
}

Material* mat_find_hash(uint32_t hash) {
    if (!hash) return NULL;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < M.n; i++)
            if (M.mats[i]->hash == hash) return M.mats[i];
        if (pass == 0 && !M.scanned) {
            M.scanned = true;
            scan(M.root);
        }
    }
    return NULL;
}

uint32_t mat_count(void) {
    if (!M.scanned) mat_find_hash(1u);
    return M.n;
}
Material* mat_at(uint32_t i) { return i < M.n ? M.mats[i] : NULL; }

void mat_mark_dirty(Material* m) {
    if (!m) return;
    m->dirty = true;
    M.any_dirty = true;
    for (uint32_t i = 0; i < M.n; i++) // its instances follow
        if (M.mats[i]->parent == m) M.mats[i]->dirty = true;
}

uint32_t mat_vpath_hash(const char* vpath) { return vpath_hash(vpath); }

// A file written by someone else (an import in the background): read again when loaded, baked again
bool mat_reload_file(const char* path_in) {
    char path[MAT_PATH];
    snprintf(path, sizeof path, "%s", path_in);
    for (char* c = path; *c; c++)
        if (*c == '/') *c = '\\';
    for (uint32_t i = 0; i < M.n; i++) {
        Material* m = M.mats[i];
        if (_stricmp(m->path, path)) continue;
        size_t len;
        char* text = read_all(path, &len);
        if (!text) return false;
        bool ok = parse(m, text, len);
        free(text);
        m->dirty = true;
        M.any_dirty = true;
        return ok;
    }
    return false;
}

Material* mat_create(const char* path, bool instance, const char* parent_path) {
    Material tmp;
    memset(&tmp, 0, sizeof tmp);
    snprintf(tmp.path, sizeof tmp.path, "%s", path);
    tmp.instance = instance;
    tmp.resolution = 1024;
    tmp.anim_frames = 16, tmp.anim_res = 512;
    tmp.opacity_clip = MAT_OPACITY_CLIP_DEFAULT;
    tmp.emissive_light = 1.0f; // (2026-11-23: its glow lights the scene)
    tmp.absorption[0] = tmp.absorption[1] = tmp.absorption[2] = 1.0f, tmp.absorption_dist = 10.0f, tmp.colored_shadow = true; // (Q4)
    tmp.disp_magnitude = 5.0f, tmp.disp_center = 0.5f; // (Q17)
    MatGraph* g = NULL;
    if (instance) {
        snprintf(tmp.parent_path, sizeof tmp.parent_path, "%s", parent_path ? parent_path : "");
    } else {
        g = calloc(1, sizeof *g);
        if (!g) return NULL;
        mat_graph_default(g);
        tmp.graph = g;
    }
    bool ok = mat_save(&tmp);
    free(g);
    return ok ? mat_load(path) : NULL;
}

#include "material_gpu.inc"
