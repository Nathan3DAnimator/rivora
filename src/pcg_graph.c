// PCG Graphs (Q15, pcg.h): the node types and their settings, the graph's editing, the .rpcg file (JSON: the nodes by
// their ids and their settings by name, so a node type or a setting added later reads old files), the assets read once
// by their path's hash, the examples (Content/PCG/Examples: a forest, rocks, rock clusters, a fence along a spline).
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "io_json.h"
#include "pcg.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"

// --- The node types ---
static const char* const g_target[] = { "Landscape", "Meshes", "Landscape and Meshes" };
static const char* const g_ops[] = { "<", "<=", ">", ">=", "==", "!=" };
static const char* const g_modes[] = { "Set", "Multiply", "Add", "Minimum", "Maximum" };
static const char* const g_prune[] = { "Largest First", "Smallest First", "Random" };
static const char* const g_mesh_modes[] = { "Pick One by Weight", "Every Entry" };
static const char* const g_spline_modes[] = { "On the Spline", "Inside the Width" };
#define NE(a) a, (uint8_t)(sizeof a / sizeof a[0])
#define PF(n, sl, d, lo, hi, f) { n, PP_FLOAT, sl, { d, 0, 0 }, lo, hi, f, NULL, 0 }
#define PI(n, sl, d, lo, hi) { n, PP_INT, sl, { d, 0, 0 }, lo, hi, "%.0f", NULL, 0 }
#define PB(n, sl, d) { n, PP_BOOL, sl, { d, 0, 0 }, 0, 1, NULL, NULL, 0 }
#define PE(n, sl, d, e) { n, PP_ENUM, sl, { d, 0, 0 }, 0, 0, NULL, NE(e) }
#define PV(n, sl, a, b, c, lo, hi, f) { n, PP_VEC3, sl, { a, b, c }, lo, hi, f, NULL, 0 }
#define PC(n, sl, a, b, c) { n, PP_COLOR, sl, { a, b, c }, 0, 1, NULL, NULL, 0 }
#define PS(n, k, sl) { n, k, sl, { 0, 0, 0 }, 0, 0, NULL, NULL, 0 }
#define ENTRY(k) PF("Weight " #k, 4 + (k - 1) * 10, k == 1 ? 1.0f : 0.0f, 0, 100, "%.2f"), PS("Mesh " #k, PP_MESH, k - 1), \
                 PS("Material " #k, PP_MATERIAL, 4 + k - 1), PC("Color " #k, 5 + (k - 1) * 10, 0.5f, 0.5f, 0.5f), \
                 PV("Offset " #k, 8 + (k - 1) * 10, 0, 0, 0, -100000, 100000, "%.0f"), PV("Scale " #k, 11 + (k - 1) * 10, 1, 1, 1, 0, 1000, "%.2f")

static const PcgNodeInfo g_nodes[PCGN_TYPE_COUNT] = {
    [PCGN_INPUT] = { "Input", "Input", "The graph's input: the points given to a Subgraph node (a graph run by a component: none).", PCGF_FAM_IO, 0, 1, { 0 }, { "Out" }, 0, { { 0 } } },
    [PCGN_OUTPUT] = { "Output", "Output", "The graph's output: what a Subgraph node returns (and the points the component shows).", PCGF_FAM_IO, 1, 0, { "In" }, { 0 }, 0, { { 0 } } },
    [PCGN_SURFACE] = { "Surface Sampler", "SurfaceSampler", "Points on the ground inside the volume: the Landscape, the meshes in it, or both (the highest). One per cell of a world grid (Points per m2 sets its size), moved inside it by the Looseness: the same points whatever the volume's or the cells' bounds.", PCGF_FAM_SOURCE, 0, 1, { 0 }, { "Out" }, 4,
        { PE("Target", 0, 0, g_target), PF("Points per m2", 1, 0.05f, 0.0001f, 100.0f, "%.4f"), PF("Point Extent", 2, 100.0f, 1.0f, 100000.0f, "%.0f cm"),
          PF("Looseness", 3, 1.0f, 0.0f, 1.0f, "%.2f") } },
    [PCGN_VOLUME] = { "Volume Sampler", "VolumeSampler", "Points filling the volume on a 3D world grid (Voxel Size), moved inside each voxel by the Looseness: floating things, fireflies, a cave's crystals.", PCGF_FAM_SOURCE, 0, 1, { 0 }, { "Out" }, 3,
        { PV("Voxel Size", 0, 400, 400, 400, 10, 100000, "%.0f cm"), PF("Looseness", 3, 0.5f, 0.0f, 1.0f, "%.2f"), PF("Point Extent", 4, 50.0f, 1.0f, 100000.0f, "%.0f cm") } },
    [PCGN_SPLINE] = { "Spline Sampler", "SplineSampler", "Points along the splines in the volume (a Rail, a River, a Spline Component; Spline: one actor by its name), every Spacing cm: on the spline (shifted by Offset, on both sides) or filling its width. Aligned to it and projected on the ground: fences, lamp posts, river rocks.", PCGF_FAM_SOURCE, 0, 1, { 0 }, { "Out" }, 8,
        { PS("Spline", PP_SPLINE, 0), PE("Mode", 0, 0, g_spline_modes), PF("Spacing", 1, 250.0f, 5.0f, 100000.0f, "%.0f cm"), PF("Offset", 2, 0.0f, -100000.0f, 100000.0f, "%.0f cm"),
          PB("Both Sides", 3, 0), PB("Project to Ground", 4, 1), PB("Align to Spline", 5, 1), PF("Width", 6, 0.0f, 0.0f, 100000.0f, "%.0f cm") } },
    [PCGN_ATTR_FILTER] = { "Attribute Filter", "AttributeFilter", "Splits the points by an attribute against a threshold: Density, X, Y, Z, Steepness, Scale, Yaw or one made by Create Attribute. Inside: those that pass; Outside: the others.", PCGF_FAM_FILTER, 1, 2, { "In" }, { "Inside", "Outside" }, 3,
        { PS("Attribute", PP_NAME, 0), PE("Operator", 0, 3, g_ops), PF("Threshold", 1, 0.5f, -1e9f, 1e9f, "%.3f") } },
    [PCGN_DENSITY_FILTER] = { "Density Filter", "DensityFilter", "Keeps the points whose density is between the bounds (Invert: the others). After a noise: the dense patches only.", PCGF_FAM_FILTER, 1, 2, { "In" }, { "Inside", "Outside" }, 3,
        { PF("Lower Bound", 0, 0.5f, 0.0f, 1.0f, "%.3f"), PF("Upper Bound", 1, 1.0f, 0.0f, 1.0f, "%.3f"), PB("Invert", 2, 0) } },
    [PCGN_STEEPNESS_FILTER] = { "Steepness Filter", "SteepnessFilter", "Keeps the points whose ground slope is between the angles (degrees from flat): trees on gentle ground, rocks on the cliffs.", PCGF_FAM_FILTER, 1, 2, { "In" }, { "Inside", "Outside" }, 2,
        { PF("Min Slope", 0, 0.0f, 0.0f, 90.0f, "%.0f deg"), PF("Max Slope", 1, 30.0f, 0.0f, 90.0f, "%.0f deg") } },
    [PCGN_LAYER_FILTER] = { "Landscape Layer Filter", "LayerFilter", "Keeps the points where a painted Landscape layer (1-3) weighs between the bounds: flowers on the painted grass, nothing on the road.", PCGF_FAM_FILTER, 1, 2, { "In" }, { "Inside", "Outside" }, 3,
        { PI("Layer", 0, 1, 1, 3), PF("Min Weight", 1, 0.5f, 0.0f, 1.0f, "%.2f"), PF("Max Weight", 2, 1.0f, 0.0f, 1.0f, "%.2f") } },
    [PCGN_DENSITY_NOISE] = { "Density Noise", "DensityNoise", "A random density per point between Min and Max, set or combined with the density it has (each point its own, by the seed).", PCGF_FAM_POINTS, 1, 1, { "In" }, { "Out" }, 3,
        { PE("Mode", 0, 1, g_modes), PF("Density Min", 1, 0.0f, 0.0f, 1.0f, "%.2f"), PF("Density Max", 2, 1.0f, 0.0f, 1.0f, "%.2f") } },
    [PCGN_SPATIAL_NOISE] = { "Spatial Noise", "SpatialNoise", "A smooth noise over the world (Noise Scale: the patches' size) into the density: clustered forests and clearings, the same across cells and volumes.", PCGF_FAM_POINTS, 1, 1, { "In" }, { "Out" }, 5,
        { PE("Mode", 0, 1, g_modes), PF("Noise Scale", 1, 3000.0f, 10.0f, 1e7f, "%.0f cm"), PI("Octaves", 2, 3, 1, 6), PF("Contrast", 3, 1.5f, 0.1f, 10.0f, "%.2f"),
          PF("Bias", 4, 0.0f, -1.0f, 1.0f, "%.2f") } },
    [PCGN_CREATE_ATTR] = { "Create Attribute", "CreateAttribute", "A named number on every point, random between Min and Max (equal: a constant). Read by the Attribute Filter.", PCGF_FAM_POINTS, 1, 1, { "In" }, { "Out" }, 3,
        { PS("Name", PP_NAME, 0), PF("Value Min", 0, 0.0f, -1e9f, 1e9f, "%.3f"), PF("Value Max", 1, 1.0f, -1e9f, 1e9f, "%.3f") } },
    [PCGN_TRANSFORM] = { "Transform Points", "TransformPoints", "Moves, turns and scales each point by a random amount between Min and Max (Uniform Scale: one factor), relative to what it has or absolute; Align to Normal tilts it with the ground.", PCGF_FAM_POINTS, 1, 1, { "In" }, { "Out" }, 9,
        { PV("Offset Min", 0, 0, 0, 0, -1e6f, 1e6f, "%.0f"), PV("Offset Max", 3, 0, 0, 0, -1e6f, 1e6f, "%.0f"), PV("Rotation Min", 6, 0, 0, 0, -360, 360, "%.0f"),
          PV("Rotation Max", 9, 0, 360, 0, -360, 360, "%.0f"), PV("Scale Min", 12, 1, 1, 1, 0, 1000, "%.2f"), PV("Scale Max", 15, 1, 1, 1, 0, 1000, "%.2f"),
          PB("Uniform Scale", 18, 1), PB("Absolute Rotation", 19, 0), PF("Align to Normal", 20, 0.0f, 0.0f, 1.0f, "%.2f") } },
    [PCGN_PROJECT] = { "Project to Surface", "ProjectToSurface", "Drops each point onto the ground under it (the Landscape, the meshes or both), its normal taken; a point with no ground is removed.", PCGF_FAM_POINTS, 1, 1, { "In" }, { "Out" }, 3,
        { PE("Target", 0, 0, g_target), PF("Height Offset", 1, 0.0f, -1e5f, 1e5f, "%.0f cm"), PF("Align to Normal", 2, 0.0f, 0.0f, 1.0f, "%.2f") } },
    [PCGN_SELF_PRUNE] = { "Self Pruning", "SelfPruning", "Removes the points that overlap another (their extent times the scale, times Radius Scale): the largest kept first, or the smallest, or at random.", PCGF_FAM_POINTS, 1, 1, { "In" }, { "Out" }, 2,
        { PE("Mode", 0, 0, g_prune), PF("Radius Scale", 1, 1.0f, 0.01f, 100.0f, "%.2f") } },
    [PCGN_DIFFERENCE] = { "Difference", "Difference", "The Source points that don't touch any Difference point (their extents times Radius Scale): bushes kept away from the trees, rocks off the road.", PCGF_FAM_POINTS, 2, 1, { "Source", "Difference" }, { "Out" }, 1,
        { PF("Radius Scale", 0, 1.0f, 0.01f, 100.0f, "%.2f") } },
    [PCGN_MERGE] = { "Merge", "Merge", "Every input's points in one set, in the inputs' order.", PCGF_FAM_POINTS, 4, 1, { "In 1", "In 2", "In 3", "In 4" }, { "Out" }, 0, { { 0 } } },
    [PCGN_MESH_SPAWNER] = { "Static Mesh Spawner", "StaticMeshSpawner", "A mesh at each point: one of the entries picked by weight, or every entry (a tree's trunk and crown, a fence's post and rails). Each entry: a mesh (a shape or an imported mesh), its material or color, an offset and a scale in the point's frame. Passes the points on.", PCGF_FAM_SPAWN, 1, 1, { "In" }, { "Out" }, 28,
        { PE("Mode", 0, 0, g_mesh_modes), PB("Collision", 1, 1), PB("Cast Shadow", 2, 1), PF("Wind", 3, 0.0f, 0.0f, 1.0f, "%.2f"), ENTRY(1), ENTRY(2), ENTRY(3), ENTRY(4) } },
    [PCGN_ACTOR_SPAWNER] = { "Spawn Actor", "SpawnActor", "An actor of a Blueprint class at each point (its BeginPlay runs while playing). Passes the points on.", PCGF_FAM_SPAWN, 1, 1, { "In" }, { "Out" }, 1,
        { PS("Actor Class", PP_CLASS, 0) } },
    [PCGN_NESTED] = { "Nested PCG", "NestedPCG", "Another PCG Graph run around each point (its box: Bounds Size, or the point's extent), with the point's seed: a camp at each clearing, a rock cluster at each spot. Passes the points on.", PCGF_FAM_SPAWN, 1, 1, { "In" }, { "Out" }, 3,
        { PS("Graph", PP_GRAPH, 0), PF("Bounds Size", 0, 0.0f, 0.0f, 1e6f, "%.0f cm"), PI("Max Points", 1, 256, 1, 4096) } },
    [PCGN_SUBGRAPH] = { "Subgraph", "Subgraph", "Another PCG Graph's nodes run here: its Input gets these points, its Output comes out. Reuse a recipe in several graphs; the parameters of the same name pass down.", PCGF_FAM_GRAPH, 1, 1, { "In" }, { "Out" }, 1,
        { PS("Graph", PP_GRAPH, 0) } },
};

const PcgNodeInfo* pcg_node_info(int type) { return type >= 0 && type < PCGN_TYPE_COUNT ? &g_nodes[type] : NULL; }
int pcg_node_type_by_id(const char* id) {
    for (int t = 0; t < PCGN_TYPE_COUNT; t++)
        if (id && !strcmp(g_nodes[t].id, id)) return t;
    return -1;
}

// --- Editing ---
void pcg_graph_clear(PcgGraph* g) {
    uint32_t h = g->hash;
    char vp[256];
    memcpy(vp, g->vpath, sizeof vp);
    memset(g, 0, sizeof *g);
    g->hash = h, g->next_id = 1;
    memcpy(g->vpath, vp, sizeof vp);
    pcg_graph_add(g, PCGN_INPUT, -320, 0);
    pcg_graph_add(g, PCGN_OUTPUT, 520, 0);
}

int pcg_graph_add(PcgGraph* g, int type, float x, float y) {
    const PcgNodeInfo* ni = pcg_node_info(type);
    if (!ni || g->nnodes >= PCG_MAX_NODES) return -1;
    PcgNode* n = &g->nodes[g->nnodes];
    memset(n, 0, sizeof *n);
    n->type = (uint16_t)type, n->id = g->next_id++, n->pos[0] = x, n->pos[1] = y;
    for (int i = 0; i < ni->nprops; i++) {
        const PcgProp* p = &ni->props[i];
        if (p->kind == PP_VEC3 || p->kind == PP_COLOR) memcpy(&n->f[p->slot], p->def, 12);
        else if (p->kind <= PP_ENUM) n->f[p->slot] = p->def[0];
    }
    if (type == PCGN_MESH_SPAWNER) snprintf(n->s[0], PCG_STR, "Cube");
    if (type == PCGN_ATTR_FILTER) snprintf(n->s[0], PCG_STR, "Density");
    if (type == PCGN_CREATE_ATTR) snprintf(n->s[0], PCG_STR, "MyAttribute");
    g->version++;
    return (int)g->nnodes++;
}

bool pcg_graph_link(PcgGraph* g, int from, int from_pin, int to, int to_pin) {
    if (from < 0 || to < 0 || from == to || (uint32_t)from >= g->nnodes || (uint32_t)to >= g->nnodes) return false;
    const PcgNodeInfo *a = pcg_node_info(g->nodes[from].type), *b = pcg_node_info(g->nodes[to].type);
    if (from_pin < 0 || from_pin >= a->nout || to_pin < 0 || to_pin >= b->nin || g->nlinks >= PCG_MAX_LINKS) return false;
    for (uint32_t i = 0; i < g->nlinks; i++) {
        const PcgLink* l = &g->links[i];
        if (l->from == from && l->from_pin == from_pin && l->to == to && l->to_pin == to_pin) return false;
    }
    // no cycle: `to` must not reach `from`
    uint8_t seen[PCG_MAX_NODES] = { 0 }, stack[PCG_MAX_NODES];
    int sp = 0;
    stack[sp++] = (uint8_t)to;
    while (sp) {
        int k = stack[--sp];
        if (k == from) return false;
        if (seen[k]) continue;
        seen[k] = 1;
        for (uint32_t i = 0; i < g->nlinks; i++)
            if (g->links[i].from == k && sp < PCG_MAX_NODES) stack[sp++] = g->links[i].to;
    }
    g->links[g->nlinks++] = (PcgLink){ (uint8_t)from, (uint8_t)from_pin, (uint8_t)to, (uint8_t)to_pin };
    g->version++;
    return true;
}

void pcg_graph_remove(PcgGraph* g, int node) {
    if (node < 0 || (uint32_t)node >= g->nnodes) return;
    uint32_t w = 0;
    for (uint32_t i = 0; i < g->nlinks; i++) {
        PcgLink l = g->links[i];
        if (l.from == node || l.to == node) continue;
        if (l.from > node) l.from--;
        if (l.to > node) l.to--;
        g->links[w++] = l;
    }
    g->nlinks = w;
    memmove(&g->nodes[node], &g->nodes[node + 1], sizeof(PcgNode) * (g->nnodes - (uint32_t)node - 1));
    g->nnodes--;
    g->version++;
}

int pcg_graph_param(const PcgGraph* g, const char* name) {
    for (uint32_t i = 0; i < g->nparams; i++)
        if (!_stricmp(g->params[i].name, name)) return (int)i;
    return -1;
}

// --- Assets ---
static char g_root[700];

uint32_t pcg_vpath_hash(const char* vpath) { // (the same as the materials' and Blueprints')
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) {
        char chr = *c == '\\' ? '/' : *c;
        h = (h ^ (uint8_t)tolower((unsigned char)chr)) * 16777619u;
    }
    return h ? h : 1u;
}
bool pcg_is_path(const char* vpath) {
    const char* d = vpath ? strrchr(vpath, '.') : NULL;
    return d && !_stricmp(d, ".rpcg");
}
static bool to_abs(const char* vpath, char* out, size_t cap) {
    if (!g_root[0] || strncmp(vpath, "/Content", 8)) return false;
    snprintf(out, cap, "%s%s", g_root, vpath + 8);
    return true;
}

uint32_t pcg_material_hash(const char* vpath) { return vpath && vpath[0] ? pcg_vpath_hash(vpath) : 0u; }
uint32_t pcg_mesh_id(const char* s) {
    static const char* const shapes[SHAPE_COUNT] = { "Cube", "Sphere", "Cylinder", "Cone", "Plane" };
    if (!s || !s[0]) return SHAPE_CUBE;
    for (int i = 0; i < SHAPE_COUNT; i++)
        if (!_stricmp(s, shapes[i])) return (uint32_t)i;
    return s[0] == '/' ? render_mesh_id_of(s) : SHAPE_CUBE;
}

bool pcg_graph_load(const char* vpath, PcgGraph* g) {
    memset(g, 0, sizeof *g);
    snprintf(g->vpath, sizeof g->vpath, "%s", vpath);
    g->hash = pcg_vpath_hash(vpath), g->next_id = 1;
    char path[1100];
    if (!to_abs(vpath, path, sizeof path)) return false;
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char* t = n > 0 && n < (8 << 20) ? malloc((size_t)n) : NULL; // (a load, not a frame)
    bool ok = t && fread(t, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    JsonDoc doc;
    char err[160];
    if (!ok || !json_parse(t, (size_t)n, &doc, err, sizeof err)) {
        free(t);
        return false;
    }
    const JsonValue* r = &doc.root;
    const JsonValue* ps = json_get(r, "params");
    for (size_t i = 0; i < json_len(ps) && g->nparams < PCG_MAX_PARAMS; i++) {
        const JsonValue* o = json_at(ps, i);
        PcgParam* p = &g->params[g->nparams++];
        snprintf(p->name, sizeof p->name, "%s", json_get_str(o, "name", "Param"));
        p->value = (float)json_get_num(o, "value", 0.0), p->mn = (float)json_get_num(o, "min", 0.0), p->mx = (float)json_get_num(o, "max", 1.0);
    }
    const JsonValue* ns = json_get(r, "nodes");
    uint32_t idx_of[PCG_MAX_NODES * 2]; // (a file's node index -> ours: unknown types skipped)
    for (size_t i = 0; i < json_len(ns) && i < PCG_MAX_NODES * 2; i++) {
        const JsonValue* o = json_at(ns, i);
        idx_of[i] = 0xFFu;
        int type = pcg_node_type_by_id(json_get_str(o, "type", ""));
        float pos[2] = { 0, 0 };
        json_get_floats(o, "pos", pos, 2);
        int k = type >= 0 ? pcg_graph_add(g, type, pos[0], pos[1]) : -1;
        if (k < 0) continue;
        idx_of[i] = (uint32_t)k;
        PcgNode* nd = &g->nodes[k];
        nd->id = (uint32_t)json_get_int(o, "id", (int)nd->id), nd->flags = (uint16_t)json_get_int(o, "flags", 0);
        const PcgNodeInfo* ni = pcg_node_info(type);
        const JsonValue* pr = json_get(o, "props");
        const JsonValue* bd = json_get(o, "bind");
        for (int pi = 0; pi < ni->nprops; pi++) {
            const PcgProp* p = &ni->props[pi];
            const JsonValue* v = json_get(pr, p->name);
            if (v) {
                if (p->kind >= PP_MESH) snprintf(nd->s[p->slot], PCG_STR, "%s", json_str(v, ""));
                else if (p->kind == PP_VEC3 || p->kind == PP_COLOR) json_get_floats(pr, p->name, &nd->f[p->slot], 3);
                else nd->f[p->slot] = (float)json_num(v, p->def[0]);
            }
            const char* bn = json_get_str(bd, p->name, "");
            if (bn[0] && p->kind <= PP_ENUM) {
                int pk = pcg_graph_param(g, bn);
                if (pk >= 0) nd->bind[p->slot] = (uint8_t)(pk + 1);
            }
        }
    }
    for (uint32_t i = 0; i < g->nnodes; i++)
        if (g->nodes[i].id >= g->next_id) g->next_id = g->nodes[i].id + 1;
    g->next_id = (uint32_t)json_get_int(r, "next_id", (int)g->next_id) > g->next_id ? (uint32_t)json_get_int(r, "next_id", 0) : g->next_id;
    const JsonValue* ls = json_get(r, "links");
    for (size_t i = 0; i < json_len(ls); i++) {
        float l[4] = { -1, -1, -1, -1 };
        const JsonValue* o = json_at(ls, i);
        for (int k = 0; k < 4; k++) l[k] = (float)json_num(json_at(o, (size_t)k), -1.0);
        if (l[0] < 0 || l[2] < 0 || l[0] >= PCG_MAX_NODES * 2 || l[2] >= PCG_MAX_NODES * 2) continue;
        uint32_t a = idx_of[(int)l[0]], b = idx_of[(int)l[2]];
        if (a != 0xFFu && b != 0xFFu) pcg_graph_link(g, (int)a, (int)l[1], (int)b, (int)l[3]);
    }
    json_free(&doc);
    free(t);
    g->version = 1;
    return true;
}

bool pcg_graph_save(const char* vpath, const PcgGraph* g) {
    char path[1100];
    if (!to_abs(vpath, path, sizeof path)) return false;
    JsonWriter w;
    jw_init(&w);
    jw_obj_begin(&w);
    jw_kint(&w, "version", 1);
    jw_kstr(&w, "type", "PCGGraph");
    jw_kint(&w, "next_id", g->next_id);
    jw_key(&w, "params");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < g->nparams; i++) {
        jw_obj_begin(&w);
        jw_kstr(&w, "name", g->params[i].name);
        jw_kfloat(&w, "value", g->params[i].value), jw_kfloat(&w, "min", g->params[i].mn), jw_kfloat(&w, "max", g->params[i].mx);
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "nodes");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < g->nnodes; i++) {
        const PcgNode* n = &g->nodes[i];
        const PcgNodeInfo* ni = pcg_node_info(n->type);
        jw_obj_begin(&w);
        jw_kstr(&w, "type", ni->id);
        jw_kint(&w, "id", n->id);
        jw_kfloats(&w, "pos", n->pos, 2);
        if (n->flags) jw_kint(&w, "flags", n->flags);
        jw_key(&w, "props");
        jw_obj_begin(&w);
        bool any_bind = false;
        for (int pi = 0; pi < ni->nprops; pi++) {
            const PcgProp* p = &ni->props[pi];
            if (p->kind >= PP_MESH) jw_kstr(&w, p->name, n->s[p->slot]);
            else if (p->kind == PP_VEC3 || p->kind == PP_COLOR) jw_kfloats(&w, p->name, &n->f[p->slot], 3);
            else jw_kfloat(&w, p->name, n->f[p->slot]);
            any_bind |= p->kind <= PP_ENUM && n->bind[p->slot] && n->bind[p->slot] <= g->nparams;
        }
        jw_obj_end(&w);
        if (any_bind) {
            jw_key(&w, "bind");
            jw_obj_begin(&w);
            for (int pi = 0; pi < ni->nprops; pi++) {
                const PcgProp* p = &ni->props[pi];
                if (p->kind <= PP_ENUM && n->bind[p->slot] && n->bind[p->slot] <= g->nparams) jw_kstr(&w, p->name, g->params[n->bind[p->slot] - 1].name);
            }
            jw_obj_end(&w);
        }
        jw_obj_end(&w);
    }
    jw_arr_end(&w);
    jw_key(&w, "links");
    jw_arr_begin(&w);
    for (uint32_t i = 0; i < g->nlinks; i++) {
        float l[4] = { g->links[i].from, g->links[i].from_pin, g->links[i].to, g->links[i].to_pin };
        jw_arr_begin(&w);
        for (int k = 0; k < 4; k++) jw_int(&w, (long long)l[k]);
        jw_arr_end(&w);
    }
    jw_arr_end(&w);
    jw_obj_end(&w);
    bool ok = false;
    if (!w.fail) {
        char dir[1100];
        snprintf(dir, sizeof dir, "%s", path);
        char* sl = strrchr(dir, '/');
        if (sl) *sl = 0, re_make_dirs(dir);
        FILE* f = re_fopen(path, "wb");
        if (f) ok = fwrite(w.buf, 1, w.len, f) == w.len, ok = fclose(f) == 0 && ok;
    }
    jw_free(&w);
    return ok;
}

// The graphs read: by hash, once (the editor's copy replaces it: pcg_graph_edited)
#define PCG_ASSETS 64
typedef struct Entry { uint32_t hash; int8_t state; char vpath[256]; PcgGraph g; } Entry; // state 0 unread, 1 ok, -1 missing
static Entry g_assets[PCG_ASSETS];
static uint32_t g_nassets;

static Entry* find(uint32_t hash) {
    for (uint32_t i = 0; i < g_nassets; i++)
        if (g_assets[i].hash == hash) return &g_assets[i];
    return NULL;
}
void pcg_graph_remember(const char* vpath) {
    uint32_t h = pcg_vpath_hash(vpath);
    if (find(h) || g_nassets >= PCG_ASSETS) return;
    Entry* e = &g_assets[g_nassets++];
    memset(e, 0, sizeof *e);
    e->hash = h;
    snprintf(e->vpath, sizeof e->vpath, "%s", vpath);
}
typedef struct Scan { const char* abs; const char* vdir; int depth; } Scan;
static void scan_dir(const char* abs_dir, const char* vdir, int depth);
static void scan_one(void* user, const ReDirEntry* d) {
    const Scan* s = user;
    char a[1100], v[600];
    snprintf(a, sizeof a, "%s/%s", s->abs, d->name);
    snprintf(v, sizeof v, "%s/%s", s->vdir, d->name);
    if (d->is_dir) scan_dir(a, v, s->depth + 1);
    else if (pcg_is_path(v)) pcg_graph_remember(v);
}
static void scan_dir(const char* abs_dir, const char* vdir, int depth) {
    if (depth > 8 || g_nassets >= PCG_ASSETS) return;
    Scan s = { abs_dir, vdir, depth };
    re_dir_walk(abs_dir, scan_one, &s);
}
static uint32_t g_scanned_for;
const PcgGraph* pcg_graph_get(uint32_t hash) {
    if (!hash) return NULL;
    Entry* e = find(hash);
    if (!e && g_scanned_for != hash && g_root[0]) {
        g_scanned_for = hash;
        scan_dir(g_root, "/Content", 0);
        e = find(hash);
    }
    if (!e) return NULL;
    if (!e->state) {
        e->state = pcg_graph_load(e->vpath, &e->g) ? 1 : -1;
        e->g.version = (uint32_t)(e - g_assets) * 1000003u + 1u; // (distinct from any other's)
    }
    return e->state > 0 ? &e->g : NULL;
}
const PcgGraph* pcg_graph_find(const char* name) {
    if (!name || !name[0]) return NULL;
    if (name[0] == '/') {
        pcg_graph_remember(name);
        return pcg_graph_get(pcg_vpath_hash(name));
    }
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t i = 0; i < g_nassets; i++) {
            const char* base = strrchr(g_assets[i].vpath, '/');
            base = base ? base + 1 : g_assets[i].vpath;
            const char* dot = strrchr(base, '.');
            if (dot && (size_t)(dot - base) == strlen(name) && !_strnicmp(base, name, strlen(name))) return pcg_graph_get(g_assets[i].hash);
        }
        if (!pass && g_root[0]) scan_dir(g_root, "/Content", 0);
    }
    return NULL;
}
const char* pcg_graph_vpath(uint32_t hash) {
    const Entry* e = hash ? find(hash) : NULL;
    return e ? e->vpath : "";
}
void pcg_graph_edited(const PcgGraph* g) {
    pcg_graph_remember(g->vpath);
    Entry* e = find(g->hash);
    if (!e) return;
    uint32_t v = e->state > 0 ? e->g.version : 0u;
    e->g = *g;
    e->g.version = v + 1u, e->state = 1;
}

bool pcg_graph_create(const char* folder_vpath, const char* name, const PcgGraph* from, char* out_vpath, uint32_t cap) {
    char vp[600], abs[1100];
    for (int i = 0; i < 1000; i++) {
        if (i) snprintf(vp, sizeof vp, "%s/%s%d.rpcg", folder_vpath, name, i + 1);
        else snprintf(vp, sizeof vp, "%s/%s.rpcg", folder_vpath, name);
        if (!to_abs(vp, abs, sizeof abs)) return false;
        if (!re_file_stat(abs, NULL, NULL, NULL)) break;
    }
    static PcgGraph g; // (64 KB: not on the stack)
    if (from) g = *from;
    else memset(&g, 0, sizeof g), pcg_graph_clear(&g);
    snprintf(g.vpath, sizeof g.vpath, "%s", vp);
    g.hash = pcg_vpath_hash(vp);
    if (!pcg_graph_save(vp, &g)) return false;
    pcg_graph_remember(vp);
    if (out_vpath) snprintf(out_vpath, cap, "%s", vp);
    return true;
}

// --- The examples ---
static void set3(float* f, float a, float b, float c) { f[0] = a, f[1] = b, f[2] = c; }
static void entry(PcgNode* n, int k, const char* mesh, const float col[3], float ox, float oy, float oz, float sx, float sy, float sz, float weight) {
    snprintf(n->s[k], PCG_STR, "%s", mesh);
    float* f = &n->f[4 + k * 10];
    f[0] = weight;
    memcpy(&f[1], col, 12);
    set3(&f[4], ox, oy, oz), set3(&f[7], sx, sy, sz);
}
static int param(PcgGraph* g, const char* name, float v, float mn, float mx) {
    PcgParam* p = &g->params[g->nparams];
    snprintf(p->name, sizeof p->name, "%s", name);
    p->value = v, p->mn = mn, p->mx = mx;
    return (int)++g->nparams;
}

// A forest: trees (trunk and crown) on the gentle ground in patches of a spatial noise, the trees apart; bushes in the
// patches' edges kept off the trees; the tree density a parameter
static void ex_forest(PcgGraph* g) {
    int dens = param(g, "Tree Density", 0.04f, 0.0f, 0.5f);
    int s = pcg_graph_add(g, PCGN_SURFACE, -40, -120);
    g->nodes[s].f[1] = 0.04f, g->nodes[s].bind[1] = (uint8_t)dens, g->nodes[s].f[2] = 180.0f;
    int sn = pcg_graph_add(g, PCGN_SPATIAL_NOISE, 200, -120);
    g->nodes[sn].f[1] = 6000.0f;
    int df = pcg_graph_add(g, PCGN_DENSITY_FILTER, 440, -120);
    g->nodes[df].f[0] = 0.45f;
    int st = pcg_graph_add(g, PCGN_STEEPNESS_FILTER, 680, -120);
    g->nodes[st].f[1] = 28.0f;
    int pr = pcg_graph_add(g, PCGN_SELF_PRUNE, 920, -120);
    int tr = pcg_graph_add(g, PCGN_TRANSFORM, 1160, -120);
    set3(&g->nodes[tr].f[9], 0, 360, 0), set3(&g->nodes[tr].f[12], 0.75f, 0.75f, 0.75f), set3(&g->nodes[tr].f[15], 1.4f, 1.4f, 1.4f);
    int sp = pcg_graph_add(g, PCGN_MESH_SPAWNER, 1400, -120);
    PcgNode* n = &g->nodes[sp];
    n->f[0] = 1.0f, n->f[3] = 0.35f; // (every entry, a little wind)
    entry(n, 0, "Cylinder", (float[3]){ 0.23f, 0.14f, 0.08f }, 0, 0, 150, 0.35f, 0.35f, 3.0f, 1.0f);
    entry(n, 1, "Cone", (float[3]){ 0.06f, 0.22f, 0.07f }, 0, 0, 480, 2.4f, 2.4f, 4.6f, 1.0f);
    int bn = pcg_graph_add(g, PCGN_DENSITY_NOISE, 680, 120);
    int bf = pcg_graph_add(g, PCGN_DENSITY_FILTER, 920, 120);
    g->nodes[bf].f[0] = 0.55f;
    int bd = pcg_graph_add(g, PCGN_DIFFERENCE, 1160, 120);
    g->nodes[bd].f[0] = 1.2f;
    int bt = pcg_graph_add(g, PCGN_TRANSFORM, 1400, 120);
    set3(&g->nodes[bt].f[9], 0, 360, 0), set3(&g->nodes[bt].f[12], 0.6f, 0.6f, 0.6f), set3(&g->nodes[bt].f[15], 1.1f, 1.1f, 1.1f);
    int bs = pcg_graph_add(g, PCGN_MESH_SPAWNER, 1640, 120);
    n = &g->nodes[bs];
    n->f[1] = 0.0f, n->f[3] = 0.6f; // (bushes: no collision, more wind)
    entry(n, 0, "Sphere", (float[3]){ 0.10f, 0.26f, 0.06f }, 0, 0, 30, 1.6f, 1.6f, 0.9f, 1.0f);
    pcg_graph_link(g, s, 0, sn, 0), pcg_graph_link(g, sn, 0, df, 0), pcg_graph_link(g, df, 0, st, 0), pcg_graph_link(g, st, 0, pr, 0);
    pcg_graph_link(g, pr, 0, tr, 0), pcg_graph_link(g, tr, 0, sp, 0), pcg_graph_link(g, sp, 0, 1, 0);
    pcg_graph_link(g, df, 1, bn, 0), pcg_graph_link(g, bn, 0, bf, 0), pcg_graph_link(g, bf, 0, bd, 0), pcg_graph_link(g, pr, 0, bd, 1);
    pcg_graph_link(g, bd, 0, bt, 0), pcg_graph_link(g, bt, 0, bs, 0);
}
// Rocks: sparse, turned every way, flattened; some mossy (an attribute) in a greener grey
static void ex_rocks(PcgGraph* g) {
    int dens = param(g, "Rock Density", 0.01f, 0.0f, 1.0f);
    int s = pcg_graph_add(g, PCGN_SURFACE, -40, 0);
    g->nodes[s].f[1] = 0.01f, g->nodes[s].f[2] = 120.0f, g->nodes[s].bind[1] = (uint8_t)dens;
    int dn = pcg_graph_add(g, PCGN_DENSITY_NOISE, 200, 0);
    g->nodes[dn].f[0] = 0.0f; // (Set)
    int df = pcg_graph_add(g, PCGN_DENSITY_FILTER, 440, 0);
    g->nodes[df].f[0] = 0.5f;
    int tr = pcg_graph_add(g, PCGN_TRANSFORM, 680, 0);
    PcgNode* t = &g->nodes[tr];
    set3(&t->f[0], 0, 0, -25), set3(&t->f[3], 0, 0, -10), set3(&t->f[6], -20, 0, -20), set3(&t->f[9], 20, 360, 20);
    set3(&t->f[12], 0.6f, 0.6f, 0.4f), set3(&t->f[15], 2.2f, 2.0f, 1.3f), t->f[18] = 0.0f, t->f[20] = 0.6f;
    int pr = pcg_graph_add(g, PCGN_SELF_PRUNE, 920, 0);
    int ca = pcg_graph_add(g, PCGN_CREATE_ATTR, 1160, 0);
    snprintf(g->nodes[ca].s[0], PCG_STR, "Moss");
    int af = pcg_graph_add(g, PCGN_ATTR_FILTER, 1400, 0);
    snprintf(g->nodes[af].s[0], PCG_STR, "Moss"), g->nodes[af].f[0] = 3.0f, g->nodes[af].f[1] = 0.65f;
    for (int k = 0; k < 2; k++) {
        int sp = pcg_graph_add(g, PCGN_MESH_SPAWNER, 1640, k ? 160.0f : -120.0f);
        PcgNode* n = &g->nodes[sp];
        const float grey[3] = { 0.36f, 0.35f, 0.33f }, moss[3] = { 0.24f, 0.30f, 0.17f };
        entry(n, 0, "Sphere", k ? grey : moss, 0, 0, 0, 1.0f, 1.0f, 0.65f, 2.0f);
        entry(n, 1, "Cube", k ? grey : moss, 0, 0, 0, 0.8f, 0.7f, 0.5f, 1.0f);
        pcg_graph_link(g, af, k, sp, 0);
        pcg_graph_link(g, sp, 0, 1, 0);
    }
    pcg_graph_link(g, s, 0, dn, 0), pcg_graph_link(g, dn, 0, df, 0), pcg_graph_link(g, df, 0, tr, 0), pcg_graph_link(g, tr, 0, pr, 0);
    pcg_graph_link(g, pr, 0, ca, 0), pcg_graph_link(g, ca, 0, af, 0);
}
// Rock clusters: a few spots, each one a Nested PCG of PCG_Rocks in a box of 16 m, denser (its Rock Density passes down)
static void ex_clusters(PcgGraph* g) {
    param(g, "Rock Density", 0.12f, 0.0f, 1.0f); // (passed down to PCG_Rocks: its parameter of the same name)
    int s = pcg_graph_add(g, PCGN_SURFACE, -40, 0);
    g->nodes[s].f[1] = 0.002f, g->nodes[s].f[2] = 800.0f;
    int pr = pcg_graph_add(g, PCGN_SELF_PRUNE, 200, 0);
    int ne = pcg_graph_add(g, PCGN_NESTED, 440, 0);
    snprintf(g->nodes[ne].s[0], PCG_STR, "/Content/PCG/Examples/PCG_Rocks.rpcg");
    g->nodes[ne].f[0] = 800.0f;
    pcg_graph_link(g, s, 0, pr, 0), pcg_graph_link(g, pr, 0, ne, 0), pcg_graph_link(g, ne, 0, 1, 0);
}
// A fence along the splines in the volume: a post every 2.5 m and two rails to the next one
static void ex_fence(PcgGraph* g) {
    int s = pcg_graph_add(g, PCGN_SPLINE, -40, 0);
    g->nodes[s].f[1] = 250.0f;
    int sp = pcg_graph_add(g, PCGN_MESH_SPAWNER, 240, 0);
    PcgNode* n = &g->nodes[sp];
    n->f[0] = 1.0f;
    const float wood[3] = { 0.30f, 0.20f, 0.11f };
    entry(n, 0, "Cylinder", wood, 0, 0, 65, 0.14f, 0.14f, 1.3f, 1.0f);
    entry(n, 1, "Cube", wood, 125, 0, 105, 2.5f, 0.07f, 0.12f, 1.0f);
    entry(n, 2, "Cube", wood, 125, 0, 55, 2.5f, 0.07f, 0.12f, 1.0f);
    pcg_graph_link(g, s, 0, sp, 0), pcg_graph_link(g, sp, 0, 1, 0);
}

static void write_example(const char* name, void (*build)(PcgGraph* g)) {
    char vp[300], abs[1100];
    snprintf(vp, sizeof vp, "/Content/PCG/Examples/%s.rpcg", name);
    if (!to_abs(vp, abs, sizeof abs)) return;
    if (re_file_stat(abs, NULL, NULL, NULL)) {
        pcg_graph_remember(vp);
        return;
    }
    static PcgGraph g;
    memset(&g, 0, sizeof g);
    snprintf(g.vpath, sizeof g.vpath, "%s", vp);
    g.hash = pcg_vpath_hash(vp);
    pcg_graph_clear(&g);
    build(&g);
    float right = 0.0f; // (the Output at the right of everything)
    for (uint32_t i = 2; i < g.nnodes; i++) right = fmaxf(right, g.nodes[i].pos[0]);
    g.nodes[1].pos[0] = right + 280.0f, g.nodes[1].pos[1] = 0.0f;
    pcg_graph_save(vp, &g);
    pcg_graph_remember(vp);
}

void pcg_init(const char* content_root, bool examples) {
    snprintf(g_root, sizeof g_root, "%s", content_root ? content_root : "");
    for (uint32_t i = 0; i < g_nassets; i++) g_assets[i].state = 0; // (a new project: read again)
    g_nassets = 0, g_scanned_for = 0;
    if (!examples || !g_root[0]) return;
    write_example("PCG_Forest", ex_forest);
    write_example("PCG_Rocks", ex_rocks);
    write_example("PCG_RockClusters", ex_clusters);
    write_example("PCG_Fence", ex_fence);
}

// A graph already read (no file touched: the workers' lookups); NULL: not read or missing
const PcgGraph* pcg_graph_cached(uint32_t hash) {
    const Entry* e = hash ? find(hash) : NULL;
    return e && e->state > 0 ? &e->g : NULL;
}
// The graphs a graph reaches (Subgraph, Nested PCG) read now, on the main thread; the sum of their versions (a change
// in any of them regenerates)
uint32_t pcg_graph_prepare(const PcgGraph* g, int depth) {
    if (!g || depth > 4) return 0;
    uint32_t v = g->version * 2654435761u + g->hash;
    for (uint32_t i = 0; i < g->nnodes; i++) {
        const PcgNode* n = &g->nodes[i];
        if ((n->type == PCGN_SUBGRAPH || n->type == PCGN_NESTED) && n->s[0][0]) {
            const PcgGraph* c = pcg_graph_find(n->s[0]);
            if (c && c != g) v += pcg_graph_prepare(c, depth + 1) * 31u;
        }
    }
    return v;
}
