// The Debug Viewer (debug_viewer.h, notas-dev/debug-viewer.md)
void mr_register_debug(void); // (Q25b: xr_mr.c)
#include "spline.h"
#include "audio.h"
#include "theme.h"
#include "audio_internal.h"
#include <math.h>
#include "debug_viewer.h"
#include "farview.h" // (Q21)
#include "pcg.h"
#include "outline.h" // (Q11)
#include "terror.h"
#include "frame_budget.h"
#include "tess.h" // (Q17)
#include "online.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "blueprint.h"
#include "physics.h"
#include "anim.h"
#include "ai.h"
#include "navmesh.h"
#include "health.h"
#include "save_progress.h"
#include "interaction.h"
#include "game_rules.h"
#include "ability.h"
#include "hud.h"
#include "fpcam.h"
#include "components.h"
#include "gi_cache.h"
#include "i18n.h"
#include "jobs.h"
#include "material.h"
#include "platform.h"
#include "render.h"
#include "systems.h"
#include "vkctx.h"
#include "quest.h"

#define DV_MAX_LISTS 32 // (Q14: 16 dropped the lists past the 16th in silence: 25 are registered now, HUD, Horror Kit, Frame Budget, Splines...)
#define DV_REFRESH 0.25 // s between two collections (the totals are smoothed every frame)

typedef struct DvSlot {
    DvCollectFn collect;
    char title[48];
    DvList list;
    bool builtin;
} DvSlot;

static struct {
    bool open, was_open;
    bool cost_on, measuring; // the object cost view (dv_costs) measures too; measuring: render_set_detail / bp_profile on
    DvSlot lists[DV_MAX_LISTS];
    int count;
    bool builtins;
    double next_collect;
    double self_collect_ms, self_ui_ms; // the viewer's own cost (smoothed)
    double frame_ms, cpu_ms, gpu_ms;    // smoothed
} D;

// The frame rate's color (the editor's status bar, this window): 10 fps strong red, 30 orange, 60 dark green, 100
// strong light green, 200 and above gold; blended between them
void dv_fps_color(double fps, float out[3]) {
    static const float at[5] = { 10, 30, 60, 100, 200 };
    static const float c[5][3] = { { 1.0f, 0.08f, 0.06f }, { 1.0f, 0.55f, 0.05f }, { 0.1f, 0.55f, 0.15f },
                                   { 0.35f, 1.0f, 0.3f }, { 1.0f, 0.8f, 0.18f } };
    int i = 0;
    while (i < 4 && fps >= at[i + 1]) i++;
    float t = i >= 4 ? 0.0f : fminf(fmaxf(((float)fps - at[i]) / (at[i + 1] - at[i]), 0.0f), 1.0f);
    int j = i < 4 ? i + 1 : 4;
    for (int k = 0; k < 3; k++) out[k] = c[i][k] + (c[j][k] - c[i][k]) * t;
}

bool dv_open(void) { return D.open; }
void dv_set_open(bool open) { D.open = open; }

void dv_register(const char* title, DvCollectFn collect) {
    for (int i = 0; i < D.count; i++)
        if (!strcmp(D.lists[i].title, title)) { // registered again: the newer collect wins
            D.lists[i].collect = collect;
            return;
        }
    if (D.count >= DV_MAX_LISTS) return;
    DvSlot* s = &D.lists[D.count++];
    memset(s, 0, sizeof *s);
    snprintf(s->title, sizeof s->title, "%s", title);
    s->collect = collect;
}

// --- Collecting: the heaviest DV_MAX_ITEMS in a min-heap while adding (millions of objects: O(log n) each) ---

static void heap_down(DvItem* h, uint32_t n, uint32_t i) {
    for (;;) {
        uint32_t l = 2 * i + 1, r = l + 1, m = i;
        if (l < n && h[l].ms < h[m].ms) m = l;
        if (r < n && h[r].ms < h[m].ms) m = r;
        if (m == i) return;
        DvItem t = h[i];
        h[i] = h[m];
        h[m] = t;
        i = m;
    }
}

void dv_add(DvList* l, const char* name, const char* info, float ms, Entity e) {
    DvItem it;
    snprintf(it.name, sizeof it.name, "%s", name ? name : "");
    snprintf(it.info, sizeof it.info, "%s", info ? info : "");
    it.ms = ms;
    it.e = e;
    if (l->n < DV_MAX_ITEMS) {
        uint32_t i = l->n++;
        l->items[i] = it;
        while (i > 0) { // up
            uint32_t p = (i - 1) / 2;
            if (l->items[p].ms <= l->items[i].ms) break;
            DvItem t = l->items[p];
            l->items[p] = l->items[i];
            l->items[i] = t;
            i = p;
        }
        return;
    }
    l->rest++;
    if (ms <= l->items[0].ms) {
        l->rest_ms += ms;
        return;
    }
    l->rest_ms += l->items[0].ms; // the lightest kept one goes to the rest
    l->items[0] = it;
    heap_down(l->items, l->n, 0);
}

static int by_ms(const void* a, const void* b) {
    float x = ((const DvItem*)a)->ms, y = ((const DvItem*)b)->ms;
    return x < y ? 1 : x > y ? -1 : 0;
}

static const char* entity_name(World* w, Entity e) {
    const CName* n = ecs_get(w, e, COMP_NAME);
    return n && n->text[0] ? n->text : "(actor)";
}

// --- The built-in lists ---

// The screen share a sphere covers (1: the camera inside it; 0: behind the camera)
static float screen_cover(const DvContext* c, const float p[3], float r) {
    float d[3] = { p[0] - c->camera[0], p[1] - c->camera[1], p[2] - c->camera[2] };
    float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (dist <= r) return 1.0f;
    if (d[0] * c->fwd[0] + d[1] * c->fwd[1] + d[2] * c->fwd[2] < -r) return 0.0f;
    float tanh_ = tanf(c->fov_deg * 0.5f * 0.017453292f), focal = c->view_w * 0.5f / fmaxf(tanh_, 1e-3f);
    float px = r / dist * focal;
    return fminf(3.14159f * px * px / fmaxf(c->view_w * c->view_h, 1.0f), 1.0f);
}

// The lights' weights (the main pass's lighting work): a light the screen share it reaches times 1 (+3 with shadow rays);
// each pixel's own work (the GI's reads, the sky, the material) counts as 4. Returns the share of the meshes' pass that
// is the lights' (both lists use it: what one counts the other doesn't)
static double light_weights(const DvContext* c, DvList* l, double meshes_ms) {
    World* w = c->world;
    double sum = 0.0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            const uint64_t need = COMP_BIT(COMP_LIGHT) | COMP_BIT(COMP_TRANSFORM);
            if ((a->mask & need) != need) continue;
            const CLight* lt = ecs_arch_column(a, COMP_LIGHT);
            const CTransform* xf = ecs_arch_column(a, COMP_TRANSFORM);
            const Entity* ents = ecs_arch_entities(a);
            for (uint32_t i = 0; i < a->count; i++) {
                if (lt[i].type == LIGHT_SKY || (lt[i].ext && !lt[i].affects_world)) continue;
                float cover = lt[i].type == LIGHT_DIRECTIONAL ? 1.0f : screen_cover(c, xf[i].location, lt[i].attenuation_radius);
                double wgt = cover * (1.0 + (lt[i].cast_shadows ? 3.0 : 0.0));
                if (pass == 0) {
                    sum += wgt;
                    continue;
                }
                char info[48];
                snprintf(info, sizeof info, TR("%.0f%% of the view%s"), cover * 100.0f, lt[i].cast_shadows ? TR(", shadow rays") : "");
                dv_add(l, entity_name(w, ents[i]), info, (float)(meshes_ms * wgt / (sum + 4.0)), ents[i]);
            }
        }
        if (!l) break;
    }
    return sum / (sum + 4.0);
}

// Meshes: the GPU cull, the shadow map and the main pass's meshes (measured) minus the lights' share of it (in Lighting),
// split per visible actor by the triangles of the LOD the cull drew (and of its shadow), its share of the screen (the
// pixels it shades) and a fixed cost per instance
static void collect_meshes(DvList* l, const DvContext* c) {
    const RenderStats* rs = render_stats();
    double shading = rs->gpu_meshes_ms * (1.0 - light_weights(c, NULL, 0.0));
    l->gpu = true;
    l->estimated = true;
    l->ms = rs->gpu_cull_ms + rs->gpu_shadow_ms + shading;
    snprintf(l->note, sizeof l->note, TR("GPU cull %.2f + shadow map %.2f + meshes %.2f ms (without the lights); per actor: triangles drawn, screen share"),
             rs->gpu_cull_ms, rs->gpu_shadow_ms, shading);
    render_request_vis();
    uint32_t n;
    const uint32_t* vis = render_debug_vis(&n);
    if (!vis) return;
    World* w = c->world;
    double total = 0.0;
    for (int pass = 0; pass < 2; pass++) {
        for (uint32_t s = 0; s < n; s++) {
            uint32_t v = vis[s], cmd = v & 0xFFFu, sh = (v >> 16) & 0xFFFu;
            if (cmd == 0xFFFu && sh == 0xFFFu) continue;
            Entity e = ecs_entity_at(w, s);
            if (e == ENTITY_NONE) continue;
            uint32_t tris = cmd != 0xFFFu ? render_lod_triangles(cmd / 4, cmd % 4) : 0;
            uint32_t stris = sh != 0xFFFu ? render_lod_triangles(sh / 4, sh % 4) : 0;
            float cover = 0.0f;
            const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
            if (t && cmd != 0xFFFu) {
                float sc = fmaxf(fabsf(t->scale[0]), fmaxf(fabsf(t->scale[1]), fabsf(t->scale[2])));
                cover = screen_cover(c, t->location, 87.0f * sc) * 0.5f; // the bounding sphere overstates a box: half
            }
            // a triangle's setup and the pixels' shading in one unit: the whole screen ~ 200 000 triangles
            double wgt = (double)tris + 0.3 * stris + 64.0 + 200000.0 * cover;
            if (pass == 0) {
                total += wgt;
                continue;
            }
            char info[48];
            if (cmd != 0xFFFu) snprintf(info, sizeof info, "LOD %u \xc2\xb7 %u tris \xc2\xb7 %.0f%%%s", cmd % 4, tris, cover * 200.0f, sh != 0xFFFu ? TR(" + shadow") : "");
            else snprintf(info, sizeof info, TR("only its shadow (%u tris)"), stris);
            dv_add(l, entity_name(w, e), info, (float)(l->ms * wgt / total), e);
        }
        if (total <= 0.0) break;
    }
}

// Lighting: the GI's and the sky's passes (measured) and each light's share of the main pass (estimated: the screen it
// reaches and its shadow rays)
static void collect_lighting(DvList* l, const DvContext* c) {
    const RenderStats* rs = render_stats();
    l->gpu = true;
    l->estimated = true;
    double lights = rs->gpu_meshes_ms * light_weights(c, NULL, 0.0);
    l->ms = rs->gpu_gi_ms + rs->gpu_atmosphere_ms + rs->gpu_weather_ms + rs->gpu_exposure_ms + lights;
    snprintf(l->note, sizeof l->note, "%s", TR("Measured: GI, sky, weather, exposure. The lights: their share of the main pass, estimated by the screen they cover and their shadow rays (\xe2\x89\x88)"));
    const GiCacheStats* cs = gi_cache_stats();
    char info[48];
    snprintf(info, sizeof info, TR("%u texels, %u per frame"), cs->texels, cs->per_frame);
    dv_add(l, TR("Surface Cache (GI)"), info, (float)rs->gpu_cache_ms, ENTITY_NONE);
    dv_add(l, TR("GI Probes"), "", (float)rs->gpu_probes_ms, ENTITY_NONE);
    dv_add(l, TR("Contact GI, Build Lighting"), "", (float)rs->gpu_gi_other_ms, ENTITY_NONE);
    dv_add(l, TR("Atmosphere (sky LUTs)"), "", (float)rs->gpu_atmosphere_ms, ENTITY_NONE);
    dv_add(l, TR("Weather (fog, clouds)"), "", (float)rs->gpu_weather_ms, ENTITY_NONE);
    dv_add(l, TR("Auto Exposure"), "", (float)rs->gpu_exposure_ms, ENTITY_NONE);
    light_weights(c, l, rs->gpu_meshes_ms);
}

// Shaders: each pass's GPU time (the materials' own cost comes with the material system: its list)
static void collect_shaders(DvList* l, const DvContext* c) {
    (void)c;
    const RenderStats* rs = render_stats();
    l->gpu = true;
    l->ms = c->gpu_ms;
    snprintf(l->note, sizeof l->note, "%s", TR("Each pass of the frame on the GPU (timestamps)"));
    dv_add(l, TR("Meshes: PBR lighting (mesh.frag)"), "", (float)rs->gpu_meshes_ms, ENTITY_NONE);
    dv_add(l, TR("Sky and grid"), "", (float)rs->gpu_sky_ms, ENTITY_NONE);
    dv_add(l, TR("GPU cull, scene and TLAS"), "cull.comp, tlas_fill.comp", (float)rs->gpu_cull_ms, ENTITY_NONE);
    dv_add(l, TR("Shadow map"), "", (float)rs->gpu_shadow_ms, ENTITY_NONE);
    dv_add(l, TR("Surface Cache (GI)"), "gi_cache.comp", (float)rs->gpu_cache_ms, ENTITY_NONE);
    dv_add(l, TR("GI Probes"), "probe_trace.comp", (float)rs->gpu_probes_ms, ENTITY_NONE);
    dv_add(l, TR("Weather (fog, clouds)"), "weather.comp", (float)rs->gpu_weather_ms, ENTITY_NONE);
    dv_add(l, TR("Atmosphere (sky LUTs)"), "atmosphere.comp", (float)rs->gpu_atmosphere_ms, ENTITY_NONE);
    dv_add(l, TR("Selection outline"), "mask.frag", (float)rs->gpu_mask_ms, ENTITY_NONE);
    dv_add(l, TR("Auto Exposure"), "exposure.comp", (float)rs->gpu_exposure_ms, ENTITY_NONE);
    dv_add(l, TR("Bloom (post processing)"), "bloom.comp", (float)rs->gpu_post_ms, ENTITY_NONE);
    dv_add(l, TR("Tone mapping, motion blur, DOF (composite)"), "composite.frag", (float)rs->gpu_composite_ms, ENTITY_NONE);
}

// GPU Passes (Unreal's "stat gpu"): every marked pass of the frame (gpu_mark: the crash markers' names), its mean GPU
// time since the last collect; their sum is the frame. Measured only while the viewer is open (vkctx_pass_timing)
static void collect_gpu_passes(DvList* l, const DvContext* c) {
    const GpuPassStat* ps;
    uint32_t nf = 0, np = vkctx_pass_stats(&ps, &nf);
    l->gpu = true;
    double tot = 0.0;
    for (uint32_t i = 0; i < np; i++)
        if (nf && ps[i].frames) {
            double ms = ps[i].ms / nf;
            tot += ms;
            if (ms >= 0.001) dv_add(l, ps[i].name, "", (float)ms, ENTITY_NONE);
        }
    l->ms = nf ? tot : c->gpu_ms;
    snprintf(l->note, sizeof l->note, TR("Each marked pass of the frame on the GPU, from its mark to the next (%u frames)"), nf);
    vkctx_pass_stats_reset();
}

// Texture streaming (material_stream.inc): each material's resident size, what the screen wants and its memory; the
// list's cost is the streaming's CPU work this frame (its GPU work is a bake tile inside the frame: "texels")
static void collect_textures(DvList* l, const DvContext* c) {
    (void)c;
    const MatStats* st = mat_stats();
    l->gpu = false;
    l->ms = st->stream_cpu_ms;
    if (!st->streaming) {
        snprintf(l->note, sizeof l->note, "%s", TR("Texture streaming off (the GPU lacks update-after-bind)"));
        return;
    }
    snprintf(l->note, sizeof l->note, TR("%.0f of %.0f MB; %u streamed, %u up / %u down, %u texels baked this frame"),
             (double)st->tex_bytes / 1048576.0, (double)st->budget / 1048576.0, st->streamed, st->ups, st->downs, st->stream_texels);
    for (uint32_t i = 0; i < mat_count(); i++) {
        const char* name;
        uint32_t cur, want, full;
        float need;
        uint64_t bytes;
        if (!mat_stream_info(i, &name, &cur, &want, &full, &need, &bytes)) continue;
        char info[48];
        snprintf(info, sizeof info, "%u -> %u / %u px, %.0f MB", cur, want, full, (double)bytes / 1048576.0);
        dv_add(l, name, info, 0.0f, ENTITY_NONE); // (memory, not time: the info says it)
    }
    // (Q18) the UDIM tiles: each its own size (only the tiles seen above the least)
    if (st->udim_tiles) {
        char nm[64];
        snprintf(nm, sizeof nm, TR("UDIM: %u tiles, %u seen, %.0f MB"), st->udim_tiles, st->udim_resident, (double)st->udim_bytes / 1048576.0);
        dv_add(l, nm, "", 0.0f, ENTITY_NONE);
    }
    for (uint32_t i = 0; i < 1024; i++) {
        const char* name;
        int udim;
        uint32_t cur, want, full;
        float need;
        if (!mat_udim_info(i, &name, &udim, &cur, &want, &full, &need)) continue;
        char nm[64], info[48];
        snprintf(nm, sizeof nm, "%s [%d]", name, udim);
        snprintf(info, sizeof info, "%u -> %u / %u px", cur, want, full);
        dv_add(l, nm, info, 0.0f, ENTITY_NONE);
    }
}

// Animation: the Skeletal Meshes' system registers the real list (anim.c) over this one
static void collect_animation(DvList* l, const DvContext* c) {
    static Entity ents[4096];
    static float ms[4096];
    static const char* state[4096];
    const AnimStats* st = anim_stats();
    l->gpu = false;
    l->ms = st->ms;
    if (!st->meshes) {
        snprintf(l->note, sizeof l->note, "%s", TR("No animated Skeletal Mesh in the scene"));
        return;
    }
    snprintf(l->note, sizeof l->note, TR("%u skeletal meshes (%u sampled this frame, the rest every 4th: not in view), %u parts"),
             st->meshes, st->sampled, st->parts);
    if (!c->playing) return;
    uint32_t n = anim_profile_items(ents, ms, state, 4096);
    for (uint32_t i = 0; i < n; i++) dv_add(l, entity_name(c->world, ents[i]), state[i], ms[i], ents[i]);
}

// Blueprints: each instance's Tick and Delays (CPU, measured while the viewer is open), the Characters' movement
static void collect_blueprints(DvList* l, const DvContext* c) {
    static BpProfileItem items[4096];
    double movement;
    uint32_t n = bp_profile_items(items, 4096, &movement);
    const BpStats* st = bp_stats();
    l->gpu = false;
    l->ms = c->playing ? st->ms_last_frame : 0.0;
    if (!c->playing) {
        snprintf(l->note, sizeof l->note, "%s", TR("The Blueprints run while playing (Alt+P)"));
        return;
    }
    snprintf(l->note, sizeof l->note, TR("%u instances, %u operations of the VM last frame"), st->instances, st->ops_last_frame);
    if (movement > 0.0) dv_add(l, TR("Character Movement"), "", (float)movement, ENTITY_NONE);
    for (uint32_t i = 0; i < n; i++) {
        char info[48];
        snprintf(info, sizeof info, "%s \xc2\xb7 %u ops", items[i].cls, items[i].ops);
        dv_add(l, entity_name(c->world, items[i].e), info, items[i].ms, items[i].e);
    }
}

// Physics (physics.c): the step's time, each body's share by its contacts
static void collect_physics(DvList* l, const DvContext* c) {
    static Entity ents[4096];
    static float ms[4096];
    static uint32_t nc[4096];
    const PhysicsStats* st = physics_stats();
    l->gpu = false;
    l->estimated = true;
    l->ms = c->playing ? st->ms : 0.0;
    if (!c->playing) {
        snprintf(l->note, sizeof l->note, "%s", TR("The rigid bodies simulate while playing (Alt+P)"));
        return;
    }
    snprintf(l->note, sizeof l->note, TR("%u bodies (%u awake), %u contacts, %u static colliders"), st->bodies, st->awake, st->contacts, st->statics);
    // (Q8) the step's phases while busy (ms a step): the first rows
    uint32_t bs = st->busy_steps ? st->busy_steps : 1u;
    for (int k = 0; k < 12 && st->busy_steps; k++) {
        const char* name = physics_phase_name(k);
        if (!name[0]) continue;
        char info[48];
        snprintf(info, sizeof info, "%s", TR("a step's phase"));
        dv_add(l, name, info, (float)(st->phase_ms[k] / bs), ENTITY_NONE);
    }
    uint32_t n = physics_profile_items(ents, ms, nc, 4096);
    for (uint32_t i = 0; i < n; i++) {
        char info[48];
        snprintf(info, sizeof info, TR("%u contacts"), nc[i]);
        dv_add(l, entity_name(c->world, ents[i]), info, ms[i], ents[i]);
    }
}

// AI (ai.c): the agents' frame (perception, trees, brains, paths); each agent with what it does
static void collect_ai(DvList* l, const DvContext* c) {
    static AiDebug d[1024];
    const AiStats* st = ai_stats();
    l->gpu = false;
    l->estimated = true;
    l->ms = c->playing ? st->ms : 0.0;
    if (!c->playing || !st->agents) {
        snprintf(l->note, sizeof l->note, "%s", TR("The AI agents think while playing (Alt+P)"));
        return;
    }
    snprintf(l->note, sizeof l->note, TR("%u agents, %u paths searched this frame"), st->agents, st->paths_this_frame);
    uint32_t n = ai_debug(d, 1024);
    for (uint32_t i = 0; i < n; i++) dv_add(l, entity_name(c->world, d[i].pawn), d[i].what, (float)(st->ms / (double)n), d[i].pawn);
}

// The game's systems (jobs.h): each stage's time (its systems run in parallel)
static void collect_systems(DvList* l, const DvContext* c) {
    (void)c;
    l->gpu = false;
    const Schedule* s = systems_schedule();
    l->ms = 0.0;
    snprintf(l->note, sizeof l->note, "%s", TR("The job system's stages (CPU, in parallel on every core)"));
    if (!s) return;
    for (int st = 0; st < s->stage_count; st++) {
        char name[64] = "";
        size_t at = 0;
        for (int i = 0; i < s->count; i++)
            if (s->stage_of[i] == st) at += (size_t)snprintf(name + at, at < sizeof name ? sizeof name - at : 0, "%s%s", at ? ", " : "", s->systems[i].name);
        l->ms += s->stage_ms[st];
        dv_add(l, name, TR("stage"), (float)s->stage_ms[st], ENTITY_NONE);
    }
}

// Audio (audio.h): the mixer's thread (its ms per device period, not the frame's), the voices, each sound playing
static void collect_audio(DvList* l, const DvContext* c) {
    (void)c;
    AudioStats st;
    audio_stats(&st);
    l->gpu = false;
    l->ms = st.mix_ms;
    snprintf(l->note, sizeof l->note, TR("Mixer thread: %.3f ms of %.1f (worst %.3f); %u real, %u virtual; peak %.1f dB, limiter %.1f dB; %s"),
             st.mix_ms, st.period_ms, st.mix_ms_max, st.real, st.virt, 20.0 * log10(fmax(fmax(st.peak[0], st.peak[1]), 1e-6)),
             st.limiter_db, st.device);
    AudioInfo info[256];
    uint32_t n = audio_list(info, 256);
    for (uint32_t i = 0; i < n; i++) {
        char name[96], what[96];
        snprintf(name, sizeof name, "%s", au_sound_name(info[i].sound));
        snprintf(what, sizeof what, "%.1f m, %.1f dB%s%s", info[i].dist / 100.0, 20.0 * log10(fmax(info[i].audible, 1e-6)),
                 info[i].virt ? ", virtual" : "", info[i].paused ? ", paused" : "");
        dv_add(l, name, what, 0.0f, (Entity)info[i].owner);
    }
}

// (Q17) Nanite Tessellation (tess.c): the dicing's GPU time (timestamps 30-31), the micro triangles, the budget's scale
static void collect_tess(DvList* l, const DvContext* c) {
    (void)c;
    const TessStats* t = tess_stats();
    l->gpu = true, l->ms = t->gpu_ms;
    if (!t->on) {
        snprintf(l->note, sizeof l->note, "%s", "No material with Enable Tessellation loaded (or r.Tessellation 0).");
        return;
    }
    char info[48];
    snprintf(info, sizeof info, "%.2f M micro, %u base", (double)t->micro / 1e6, t->base);
    dv_add(l, "Dicing (tess.comp)", info, (float)t->gpu_ms, ENTITY_NONE);
    snprintf(l->note, sizeof l->note, "%s: %.1f px a micro edge x %.2f (budget), up to %.0f a base edge, %u fallbacks; the micro triangles drawn are in the main pass.",
             t->subdiv ? "Diced" : "Displaced only (no DAG drawn, or Low)", t->px, t->scale, t->max_factor, t->fallback);
}

// (Q21) the distant view (farview.c): the terrain's horizon pass (timestamps 32-33, only when the sun turned), the
// impostors baked, their atlases
static void collect_farview(DvList* l, const DvContext* c) {
    (void)c;
    const FarviewStats* v = farview_stats();
    l->gpu = true, l->ms = v->shadow_gpu_ms;
    char info[96];
    snprintf(info, sizeof info, "%u x %u, %.1f m a texel, %u passes", v->res, v->res, v->texel_cm * 0.01f, v->shadow_runs);
    dv_add(l, v->shadow_on ? "Far Shadow (farshadow.comp)" : "Far Shadow: off (no Landscape, or r.FarShadow 0)", info, (float)v->shadow_gpu_ms, ENTITY_NONE);
    snprintf(info, sizeof info, "%u of %u, %u x %u, %.0f MB", v->impostors, v->candidates, v->atlas, v->atlas, (double)v->bytes / 1048576.0);
    dv_add(l, "Impostors", info, 0.0f, ENTITY_NONE);
    snprintf(l->note, sizeof l->note, "Impostors under %.0f px of screen radius (bakes %.1f ms in all); the fog's sky share %.2f. The cards are drawn in the main pass.",
             v->switch_px, v->bake_ms_total, v->fog_sky);
}

static void builtins(void) {
    if (D.builtins) return;
    D.builtins = true;
    // registered with their English titles (translated when drawn); a system's own list comes after them
    dv_register("Meshes", collect_meshes);
    dv_register("Lighting", collect_lighting);
    dv_register("Shaders", collect_shaders);
    dv_register("GPU Passes", collect_gpu_passes);
    dv_register("Texture Streaming", collect_textures);
    dv_register("Animation", collect_animation);
    dv_register("Blueprints", collect_blueprints);
    dv_register("Physics", collect_physics);
    dv_register("AI", collect_ai);
    dv_register("Audio", collect_audio);
    dv_register("Gameplay Systems", collect_systems);
    online_register_debug(); // (online.c: the network's CPU, each player's round trip, loss, bandwidth)
    navmesh_register_debug(); // (navmesh.c: the tiles, the builds, the queries)
    health_register_debug(); // (health.c: the update, the damage, each actor's health)
    progress_register_debug(); // (save_progress.c: the last save and load, the checkpoints, auto save)
    interaction_register_debug(); // (interaction.c: the focus walk, each Interactable)
    quest_register_debug(); // (quest.c: the tick, each active quest)
    rules_register_debug(); // (game_rules.c: the match, each player's score and lives)
    abil_register_debug(); // (ability.c: the effects' time, periods, casts and cooldowns; each Ability System)
    mr_register_debug();   // (Q25b, xr_mr.c: mixed reality's tick and the renderer's feed; the room's actors)
    outline_register_debug(); // (Q11, outline.c: the outline pass's GPU time, its actors)
    fpcam_register_debug(); // (Q12, fpcam.c: the player's First Person Camera and the Camera Shakes)
    pcg_register_debug(); // (Q15, pcg_world.c: the PCG components' units, generation and spawning)
    hud_register_debug(); // (hud.c: the update, the pieces drawn, each marker)
    terror_register_debug(); // (Q13, terror.c: the horror kit's frame, the tension, the filters on, the flickering lights)
    spline_register_debug(); // (Q14, spline_world.c: the splines' update, each cable's simulation)
    fb_register_debug(); // (frame_budget.c: the 10 ms split in slices, each against its goal)
    dv_register("Tessellation", collect_tess); // (Q17, tess.c: the dicing, the micro triangles, the budget)
    dv_register("Distant View", collect_farview); // (Q21, farview.c: the far shadow's horizon, the impostors)
    for (int i = 0; i < D.count; i++) D.lists[i].builtin = true;
}

// --- Drawing ---

static ImVec4 heat_color(double ms, double ref) {
    if (ms >= ref) return THV(TK_RED);        // heavy: red
    if (ms >= ref * 0.4) return THV(TK_AMBER);  // worth a look: yellow
    return THV(TK_BLUE_L);
}

static Entity draw_list(DvSlot* s, double frame_ms, bool editor) {
    DvList* l = &s->list;
    Entity picked = ENTITY_NONE;
    // a list is heavy (red) past a third of the frame or 1 ms; an item past 40% of its list (and 0.1 ms) or 0.5 ms
    double list_ref = fmax(1.0, frame_ms * 0.33);
    char head[160];
    snprintf(head, sizeof head, "%s###dv_%s", s->builtin ? TR(s->title) : s->title, s->title);
    igPushStyleColor_Vec4(ImGuiCol_Text, heat_color(l->ms, list_ref));
    bool open = igCollapsingHeader_TreeNodeFlags(head, 0);
    igPopStyleColor(1);
    // the cost in front of the title, right-aligned, with a bar of its share of the frame
    igSameLine(igGetWindowWidth() - igGetFontSize() * 11.0f, -1);
    char cost[48];
    snprintf(cost, sizeof cost, "%s%.3f ms %s", l->estimated ? "\xe2\x89\x88" : "", l->ms, l->gpu ? "GPU" : "CPU");
    igTextColored(heat_color(l->ms, list_ref), "%s", cost);
    if (!open) return ENTITY_NONE;
    if (l->note[0]) igTextDisabled("%s", l->note);
    if (l->n == 0) return ENTITY_NONE;
    ImGuiTableFlags tf = ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_ScrollY | ImGuiTableFlags_SizingStretchProp;
    float rows = (float)(l->n + (l->rest ? 1 : 0));
    float h = fminf(rows, 12.0f) * igGetTextLineHeightWithSpacing() + igGetFrameHeight();
    char tid[64];
    snprintf(tid, sizeof tid, "dvt_%s", s->title);
    if (igBeginTable(tid, 4, tf, (ImVec2){ 0, h }, 0)) {
        igTableSetupScrollFreeze(0, 1);
        igTableSetupColumn(TR("Name"), 0, 3.0f, 0);
        igTableSetupColumn(TR("Detail"), 0, 3.0f, 0);
        igTableSetupColumn("ms", 0, 1.2f, 0);
        igTableSetupColumn("%", 0, 0.8f, 0);
        igTableHeadersRow();
        ImGuiListClipper* clip = ImGuiListClipper_ImGuiListClipper();
        ImGuiListClipper_Begin(clip, (int)rows, -1);
        while (ImGuiListClipper_Step(clip)) {
            for (int r = clip->DisplayStart; r < clip->DisplayEnd; r++) {
                igTableNextRow(0, 0);
                if (r == (int)l->n) { // the rest, summed
                    igTableNextColumn();
                    igTextDisabled(TR("+ %u more"), l->rest);
                    igTableNextColumn();
                    igTableNextColumn();
                    igTextDisabled("%.3f", l->rest_ms);
                    continue;
                }
                const DvItem* it = &l->items[r];
                double item_ref = fmax(0.5, fmax(l->ms * 0.4, 0.1));
                ImVec4 col = heat_color(it->ms, item_ref);
                igTableNextColumn();
                igPushID_Int(r);
                igPushStyleColor_Vec4(ImGuiCol_Text, col);
                if (igSelectable_Bool(it->name, false, ImGuiSelectableFlags_SpanAllColumns, (ImVec2){ 0, 0 }) && editor && it->e != ENTITY_NONE)
                    picked = it->e;
                igPopStyleColor(1);
                igPopID();
                igTableNextColumn();
                igTextDisabled("%s", it->info);
                igTableNextColumn();
                igTextColored(col, "%s%.3f", l->estimated ? "\xe2\x89\x88" : "", it->ms);
                igTableNextColumn();
                igTextDisabled("%.0f%%", l->ms > 0.0 ? 100.0 * it->ms / l->ms : 0.0);
            }
        }
        ImGuiListClipper_destroy(clip);
        igEndTable();
    }
    return picked;
}

// Measuring on while the viewer is open or the object cost view is on (the renderer's finer timestamps and read-backs,
// the Blueprints' timers); off otherwise
static void update_measure(void) {
    bool want = D.open || D.cost_on;
    if (want == D.measuring) return;
    render_set_detail(want);
    bp_profile(want);
    if (!getenv("REALENGINE_GPU_PASSES")) vkctx_pass_timing(want); // (GPU Passes: each marked pass timed while open)
    D.measuring = want;
    D.was_open = D.open;
    D.next_collect = 0.0;
}

// Every list collected anew (a few times a second)
static void collect_all(const DvContext* c, double t0) {
    D.next_collect = t0 + DV_REFRESH;
    for (int i = 0; i < D.count; i++) {
        DvList* l = &D.lists[i].list;
        l->n = l->rest = 0;
        l->rest_ms = l->ms = 0.0;
        l->note[0] = 0;
        l->estimated = false;
        D.lists[i].collect(l, c);
        qsort(l->items, l->n, sizeof(DvItem), by_ms);
    }
    double ms = (re_time_seconds() - t0) * 1000.0;
    D.self_collect_ms = D.self_collect_ms > 0.0 ? D.self_collect_ms * 0.7 + ms * 0.3 : ms;
}

bool dv_costs(const DvContext* c, bool on) {
    D.cost_on = on;
    update_measure();
    if (!on) return false;
    builtins();
    double t0 = re_time_seconds();
    if (t0 < D.next_collect) return false;
    collect_all(c, t0);
    return true;
}

int dv_list_count(void) { return D.count; }
const DvList* dv_list_at(int i, const char** title) {
    if (i < 0 || i >= D.count) return NULL;
    if (title) *title = D.lists[i].title;
    return &D.lists[i].list;
}

Entity dv_frame(const DvContext* c) {
    // closed (and the object cost view off): nothing measured (the renderer's finer timestamps and read-backs, the
    // Blueprints' timers off)
    update_measure();
    if (!D.open) return ENTITY_NONE;
    builtins();
    double t0 = re_time_seconds();
    D.frame_ms = D.frame_ms > 0.0 ? D.frame_ms * 0.9 + c->frame_ms * 0.1 : c->frame_ms;
    D.cpu_ms = D.cpu_ms > 0.0 ? D.cpu_ms * 0.9 + c->cpu_ms * 0.1 : c->cpu_ms;
    D.gpu_ms = D.gpu_ms > 0.0 ? D.gpu_ms * 0.9 + c->gpu_ms * 0.1 : c->gpu_ms;
    if (t0 >= D.next_collect) collect_all(c, t0);
    double t1 = re_time_seconds();
    Entity picked = ENTITY_NONE;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 44.0f, igGetFontSize() * 36.0f }, ImGuiCond_FirstUseEver);
    igSetNextWindowPos((ImVec2){ igGetFontSize() * 4.0f, igGetFontSize() * 5.0f }, ImGuiCond_FirstUseEver, (ImVec2){ 0, 0 });
    char title[96];
    snprintf(title, sizeof title, "%s###debug_viewer", TR("Debug Viewer"));
    bool open = D.open;
    if (igBegin(title, &open, 0)) {
        double fps = D.frame_ms > 0.0 ? 1000.0 / D.frame_ms : 0.0;
        float fc[3];
        dv_fps_color(fps, fc);
        igTextColored((ImVec4){ fc[0], fc[1], fc[2], 1 }, "%.0f fps", fps);
        igSameLine(0, 12);
        igText(TR("frame %.2f ms \xc2\xb7 CPU %.2f ms \xc2\xb7 GPU %.2f ms"), D.frame_ms, D.cpu_ms, D.gpu_ms);
        // its own cost: the collection (a few times a second, here averaged per frame) and this window
        double per_frame = D.self_collect_ms * (D.frame_ms / 1000.0) / DV_REFRESH + D.self_ui_ms;
        igTextDisabled(TR("Debug Viewer itself: %.3f ms CPU a frame (collect %.2f ms every %.0f ms, window %.3f ms); nothing while closed"),
                       per_frame, D.self_collect_ms, DV_REFRESH * 1000.0, D.self_ui_ms);
        igSeparator();
        for (int i = 0; i < D.count; i++) {
            Entity e = draw_list(&D.lists[i], D.frame_ms, c->editor);
            if (e != ENTITY_NONE) picked = e;
        }
    }
    igEnd();
    D.open = open;
    double ms = (re_time_seconds() - t1) * 1000.0;
    D.self_ui_ms = D.self_ui_ms > 0.0 ? D.self_ui_ms * 0.9 + ms * 0.1 : ms;
    return picked;
}

void dv_dump(void) {
    for (int i = 0; i < D.count; i++) {
        const DvList* l = &D.lists[i].list;
        printf("[RealEngine] Debug Viewer: %s %s%.3f ms %s (%u items)\n", D.lists[i].title, l->estimated ? "~" : "", l->ms,
               l->gpu ? "GPU" : "CPU", l->n + l->rest);
        for (uint32_t k = 0; k < l->n && k < 4; k++)
            printf("[RealEngine]     %-28s %8.3f ms  %s\n", l->items[k].name, l->items[k].ms, l->items[k].info);
    }
    printf("[RealEngine] Debug Viewer itself: collect %.3f ms every %.0f ms, window %.3f ms a frame\n", D.self_collect_ms,
           DV_REFRESH * 1000.0, D.self_ui_ms);
}
