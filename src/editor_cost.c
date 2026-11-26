// The object cost view (sugestão #65, notas-dev/debug-viewer.md): View Mode > Object Cost colors each object of the 3D
// view by what it costs (green light, yellow, red heavy) and the panel at the viewport's bottom right tells, for the
// object under the mouse (else the one in the middle), what weighs in it: its meshes (triangles, LOD, shadow), its
// animation, its Blueprint, its physics, its AI, its light (a light actor), its material's textures, its GI.
// The costs are the Debug Viewer's (debug_viewer.c: the same measures and models, per actor), collected a few times a
// second while the view is on (the viewer closed too) and summed per actor (a component's to its actor). Each actor's
// total goes into a float per scene slot (render_object_cost_map) that mesh.frag colors by: only the slots written last
// time are cleared, nothing per frame but the panel.
#include "editor.h"

#include "theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "blueprint.h"
#include "debug_viewer.h"
#include "i18n.h"
#include "material.h"
#include "render.h"

// the categories, in the panel's order; a list's English title -> its category
enum { CC_MESH, CC_ANIM, CC_BP, CC_PHYS, CC_AI, CC_LIGHT, CC_CS, CC_COUNT };
static const char* const g_cat_names[CC_COUNT] = { "Meshes", "Animation", "Blueprint", "Physics", "AI", "Light", "Construction Script" };
static int category_of(const char* title) {
    if (!strcmp(title, "Meshes")) return CC_MESH;
    if (!strcmp(title, "Animation")) return CC_ANIM;
    if (!strcmp(title, "Blueprints")) return CC_BP;
    if (!strcmp(title, "Physics")) return CC_PHYS;
    if (!strcmp(title, "AI")) return CC_AI;
    if (!strcmp(title, "Lighting")) return CC_LIGHT;
    if (!strcmp(title, "Construction Scripts")) return CC_CS;
    return -1; // (the passes, the stages: not an object's)
}

#define HASH_CAP 8192u // a power of two: the actors with a cost (the lists keep the heaviest 256 each)
typedef struct CostEntry {
    Entity actor;
    float ms[CC_COUNT];
    char mesh_info[48]; // the Meshes list's detail (LOD, triangles, screen share)
} CostEntry;

static struct {
    CostEntry h[HASH_CAP];
    uint32_t used;
    uint32_t* written; // the slots written last time (cleared next time): at most the entities
    uint32_t nwritten, cap_written;
    Entity hover;
    double hover_t;
    bool on;
} K;

static CostEntry* entry(Entity actor, bool add) {
    uint32_t i = (actor * 2654435761u) & (HASH_CAP - 1);
    for (uint32_t n = 0; n < HASH_CAP; n++, i = (i + 1) & (HASH_CAP - 1)) {
        if (K.h[i].actor == actor) return &K.h[i];
        if (K.h[i].actor == ENTITY_NONE) {
            if (!add || K.used + 1 >= HASH_CAP * 3 / 4) return NULL;
            K.used++;
            memset(&K.h[i], 0, sizeof K.h[i]);
            K.h[i].actor = actor;
            return &K.h[i];
        }
    }
    return NULL;
}

static float total_ms(const CostEntry* c) {
    float t = 0.0f;
    for (int k = 0; k < CC_COUNT; k++) t += c->ms[k];
    return t;
}

// 0.01 ms green .. 0.07 yellow .. 0.5 ms and more red (a log scale: the Debug Viewer's "heavy" item is 0.5 ms)
static float heat_of(float ms) {
    if (ms <= 0.01f) return 0.0f;
    float t = log2f(ms / 0.01f) / log2f(50.0f);
    return t > 1.0f ? 1.0f : t;
}

static void put_slot(float* map, uint32_t n, uint32_t slot, float v) {
    if (slot >= n) return;
    map[slot] = v;
    if (K.nwritten == K.cap_written) { // (grows a few times, when the view is first used on a big level)
        uint32_t nc = K.cap_written ? K.cap_written * 2 : 4096;
        uint32_t* q = realloc(K.written, (size_t)nc * 4u);
        if (!q) return;
        K.written = q, K.cap_written = nc;
    }
    K.written[K.nwritten++] = slot;
}

// The lists just collected, summed per actor, into the map
static void rebuild(Editor* ed) {
    World* w = &ed->world;
    memset(K.h, 0, sizeof K.h);
    K.used = 0;
    for (int li = 0; li < dv_list_count(); li++) {
        const char* title = NULL;
        const DvList* l = dv_list_at(li, &title);
        int cat = l && title ? category_of(title) : -1;
        if (cat < 0) continue;
        for (uint32_t i = 0; i < l->n; i++) {
            const DvItem* it = &l->items[i];
            if (it->e == ENTITY_NONE || !ecs_alive(w, it->e)) continue;
            CostEntry* c = entry(bp_component_owner(w, it->e), true);
            if (!c) continue;
            c->ms[cat] += it->ms;
            if (cat == CC_MESH && !c->mesh_info[0]) snprintf(c->mesh_info, sizeof c->mesh_info, "%s", it->info);
        }
    }
    uint32_t n = 0;
    float* map = render_object_cost_map(ed->vk, &n);
    if (!map) return;
    static bool told; // (once a run: the tests read it)
    if (!told) told = true, printf("[RealEngine] Object cost: %u actors with a cost\n", K.used);
    for (uint32_t i = 0; i < K.nwritten; i++)
        if (K.written[i] < n) map[K.written[i]] = 0.0f;
    K.nwritten = 0;
    // the actors' own slots, then their components' (a Blueprint's meshes, a skeletal mesh's parts)
    for (uint32_t i = 0; i < HASH_CAP; i++)
        if (K.h[i].actor != ENTITY_NONE) put_slot(map, n, entity_index(K.h[i].actor), heat_of(total_ms(&K.h[i])));
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count; r++) {
            const CostEntry* c = entry(at[r].owner, false);
            if (c) put_slot(map, n, entity_index(ents[r]), heat_of(total_ms(c)));
        }
    }
    if (getenv("RE_COST_DEBUG")) // DEBUG
        for (uint32_t i = 0; i < K.nwritten; i++) printf("[cost] slot %u = %.3f (used %u)\n", K.written[i], map[K.written[i]], K.used);
}

void editor_cost_set_hover(Entity e) { K.hover = e; }
bool editor_cost_on(void) { return K.on; }

// A row of the panel: the name, a bar of its share, the ms
static void row(const char* name, float ms, float total, const char* info) {
    float fs = igGetFontSize();
    igTextUnformatted(name, NULL);
    igSameLine(fs * 8.5f, 0);
    ImVec2 p = igGetCursorScreenPos();
    float bw = fs * 7.0f, share = total > 0.0f ? ms / total : 0.0f, ht = heat_of(ms);
    ImDrawList* dl = igGetWindowDrawList();
    ImDrawList_AddRectFilled(dl, (ImVec2){ p.x, p.y + 3 }, (ImVec2){ p.x + bw, p.y + fs - 2 }, TH(TK_N7), 2.0f, 0);
    ImU32 col = ht < 0.5f ? igColorConvertFloat4ToU32((ImVec4){ 0.12f + 1.72f * ht, 0.72f + 0.24f * ht, 0.22f - 0.2f * ht, 1 })
                          : igColorConvertFloat4ToU32((ImVec4){ 0.98f, 0.84f - 1.44f * (ht - 0.5f), 0.12f - 0.08f * (ht - 0.5f), 1 });
    ImDrawList_AddRectFilled(dl, (ImVec2){ p.x, p.y + 3 }, (ImVec2){ p.x + bw * share, p.y + fs - 2 }, col, 2.0f, 0);
    igDummy((ImVec2){ bw, fs });
    igSameLine(0, fs * 0.5f);
    igText("%.3f ms", ms);
    if (info && info[0]) {
        igSameLine(0, fs * 0.6f);
        igTextDisabled("%s", info);
    }
}

void editor_cost_frame(Editor* ed, const DvContext* c) {
    K.on = ed->view_mode == VIEW_OBJECT_COST && !ed->game_view;
    if (dv_costs(c, K.on)) rebuild(ed);
    if (!K.on || ed->vp_rect[2] < 50.0f) return;
    // the panel at the viewport's bottom right (Unreal's stat overlays' corner)
    World* w = &ed->world;
    float fs = igGetFontSize();
    igSetNextWindowPos((ImVec2){ ed->vp_rect[0] + ed->vp_rect[2] - fs, ed->vp_rect[1] + ed->vp_rect[3] - fs }, ImGuiCond_Always, (ImVec2){ 1, 1 });
    igSetNextWindowBgAlpha(0.88f);
    ImGuiWindowFlags wf = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoInputs |
                          ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoDocking;
    if (!igBegin("##object_cost", NULL, wf)) {
        igEnd();
        return;
    }
    // the legend: the colors' scale
    ImVec2 p = igGetCursorScreenPos();
    ImDrawList* dl = igGetWindowDrawList();
    float lw = fs * 16.0f;
    ImDrawList_AddRectFilledMultiColor(dl, p, (ImVec2){ p.x + lw * 0.5f, p.y + fs * 0.5f }, TH(TK_GREEN), TH(TK_AMBER), TH(TK_AMBER), TH(TK_GREEN));
    ImDrawList_AddRectFilledMultiColor(dl, (ImVec2){ p.x + lw * 0.5f, p.y }, (ImVec2){ p.x + lw, p.y + fs * 0.5f }, TH(TK_AMBER), TH(TK_RED), TH(TK_RED), TH(TK_AMBER));
    igDummy((ImVec2){ lw, fs * 0.5f });
    igTextDisabled("0.01 ms");
    igSameLine(lw * 0.5f - fs, 0);
    igTextDisabled("0.07");
    igSameLine(lw - fs * 2.2f, 0);
    igTextDisabled("0.5 ms+");
    igSeparator();
    Entity a = ecs_alive(w, K.hover) ? bp_component_owner(w, K.hover) : ENTITY_NONE;
    if (a == ENTITY_NONE) {
        igTextDisabled("%s", TR("Point at an object to see what it costs."));
        igEnd();
        return;
    }
    const CName* nm = ecs_get(w, a, COMP_NAME);
    const CostEntry* ce = entry(a, false);
    float total = ce ? total_ms(ce) : 0.0f;
    igText("%s", nm ? nm->text : "?");
    igSameLine(0, fs);
    igTextColored(total >= 0.5f ? THV(TK_ERROR) : total >= 0.07f ? THV(TK_WARN) : THV(TK_OK),
                  "\xe2\x89\x88%.3f ms", total);
    if (!ce || total <= 0.0f) igTextDisabled("%s", TR("Light: not among the heaviest of any list."));
    for (int k = 0; ce && k < CC_COUNT; k++)
        if (ce->ms[k] > 0.0f) row(TR(g_cat_names[k]), ce->ms[k], total, k == CC_MESH ? ce->mesh_info : NULL);
    // what isn't timed per object: its material's textures and how its GI is made
    const CStaticMesh* sm = ecs_get(w, a, COMP_STATIC_MESH);
    const CTransform* xf = ecs_get(w, a, COMP_TRANSFORM);
    if (sm && sm->material) {
        const Material* m = mat_find_hash(sm->material);
        if (m) {
            int res = m->resolution > 0 ? m->resolution : 512;
            double mb = (double)res * res * 8.0 * 1.33 / (1024.0 * 1024.0); // base color + normal/rough/metal, RGBA8, mips
            igTextDisabled(TR("Material %s: textures %d\xc2\xb2 (%.1f MB), 2-3 reads a pixel"), m->name, res, mb);
        }
    }
    if (xf)
        igTextDisabled("%s", TR((xf->flags & TRANSFORM_MOBILITY_MASK) == MOBILITY_STATIC ? TR("GI: Static (lightmap when built, else the surface cache)")
                                                                              : "GI: Movable (the surface cache and the probes, every frame)"));
    igEnd();
}
