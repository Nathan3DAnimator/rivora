#include "lights.h"
#include "terror.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "editor.h"
#include "gi.h"
#include "material.h"
#include "mem.h"
#include "render.h"
#include "vkctx.h"

#define DEG2RAD 0.017453292519943295f
// The grid (shaders/lights.glsl): three levels of 64 x 64 x 16 cells, 4 m / 16 m / 64 m
#define LG_LEVELS 3
#define LG_CELLS (LG_LEVELS * 65536u)
#define LG_CELL0_CM 400.0f
// A light goes in a coarser level while its range is at least this much of the level's cell (its LOD: smaller ones
// stop lighting past the finer level, 128 m and 512 m from the camera)
#define LG_LOD 0.25f
// The items (a light in a cell): 8 M, 32 MB; past it a cell keeps what fit (lights_grid_items says when)
#define LG_ITEMS_CAP (8u << 20)

enum { LS_LOCAL = 1, LS_GLOBAL = 2, LS_DYNAMIC = 4, LS_AREA = 8, LS_CAST = 16 }; // (LS_AREA: an area light casting shadows; LS_CAST: any local one)

static struct {
    VkPipeline grid_pipe;
    GpuBuf lights; // device: a GpuLight per slot, all 0xFF bytes when empty
    uint32_t cap;
    uint8_t* state; // per slot: LS_
    GpuBuf stage[RE_FRAMES_IN_FLIGHT];
    uint32_t stage_cap;
    VkBufferCopy* regions; // this frame's (the frame arena)
    uint32_t nregions;
    // the Movable (and Rotating) local lights: compared with their last copy each frame
    uint32_t *dyn, ndyn, dyn_cap;
    GpuLight* dyn_copy;
    // the directional and sky lights, in slot order
    uint32_t *globals, nglobals, globals_cap;
    uint32_t local_count;
    uint32_t area_count; // the local lights with LS_AREA
    uint32_t cast_count; // the local lights with LS_CAST (render.c: the many lights' sampling and the split's filter run only with one)
    // the local lights casting shadows, as last sent (lsm.c picks the shadow maps' lights among them): cast_at[slot] is
    // its index (UINT32_MAX: none)
    uint32_t *cast_slots, *cast_at, ncast, cast_cap;
    GpuLight* cast_gl;
    uint32_t sel_slot; // the selection's last copy sent (Details edits it in place)
    GpuLight sel_copy;
    // the grid
    GpuBuf cells, cursor, items, read[RE_FRAMES_IN_FLIGHT];
    uint32_t slots;
    int32_t origin[LG_LEVELS][3], built_origin[LG_LEVELS][3];
    uint32_t built_slots;
    bool grid_valid;
} LS;

// The slot's LS_AREA from the light just sent (g NULL: none there): a sphere of Source Radius or a rectangle casting
// shadows, whose rays render.c filters (the shadow split)
static void area_mark(uint32_t s, const GpuLight* g) {
    bool a = g && (g->type & GPU_LIGHT_CASTS) && ((g->type & 3u) == 3u || ((g->type & 3u) != 0u && g->source_radius >= 0.5f));
    bool was = (LS.state[s] & LS_AREA) != 0;
    if (a != was) LS.state[s] ^= LS_AREA, LS.area_count += a ? 1u : (uint32_t)-1;
    bool c = g && (g->type & GPU_LIGHT_CASTS) && (g->type & 3u) != 0u, wc = (LS.state[s] & LS_CAST) != 0;
    if (c != wc) LS.state[s] ^= LS_CAST, LS.cast_count += c ? 1u : (uint32_t)-1;
    if (!LS.cast_at) return;
    uint32_t at = LS.cast_at[s];
    if (c) { // (in the list, its copy up to date)
        if (at == UINT32_MAX) {
            if (LS.ncast >= LS.cast_cap) {
                uint32_t nc = LS.cast_cap ? LS.cast_cap * 2 : 64;
                uint32_t* ns = realloc(LS.cast_slots, sizeof(uint32_t) * nc); // (the set grew: an edit, not a frame)
                if (!ns) return;
                LS.cast_slots = ns;
                GpuLight* ng = realloc(LS.cast_gl, sizeof(GpuLight) * nc);
                if (!ng) return;
                LS.cast_gl = ng;
                LS.cast_cap = nc;
            }
            at = LS.ncast++;
            LS.cast_at[s] = at;
            LS.cast_slots[at] = s;
        }
        LS.cast_gl[at] = *g;
    } else if (at != UINT32_MAX) { // (out: the last one in its place)
        uint32_t last = --LS.ncast;
        if (at != last) {
            LS.cast_slots[at] = LS.cast_slots[last];
            LS.cast_gl[at] = LS.cast_gl[last];
            LS.cast_at[LS.cast_slots[at]] = at;
        }
        LS.cast_at[s] = UINT32_MAX;
    }
}

static GpuLight none_light(void) {
    GpuLight g;
    memset(&g, 0xFF, sizeof g); // type ~0u: empty
    return g;
}

// (2026-11-23, "objetos com material emissivo iluminem e também tenham luz indireta") An actor whose material glows
// lights the scene as a point light in its own slot (a mesh has no CLight there): at its bounds' center, a sphere of their
// radius, the candela its emitted power gives (the emission's baked mean in nits x its surface in m2 / 4: a glowing
// sphere's, an even guess for any shape), times the material's Emissive Light. Unshadowed unless the material's Emissive
// Shadows: then by ray only, from its sphere out (half_h = -its radius: rt.glsl light_skip), never a shadow map (from
// inside its own mesh it would see nothing else); without ray tracing it lights unshadowed. It is always in the compared set: a material edit, a move, a hidden actor
// show the next frame. Under EMIT_MIN_CD (a tiny LED) the GI's rays alone see its glow
#define EMIT_MIN_CD 0.5f
#define EMIT_MIN_LUX 0.5f   // its reach: where it gives this much
#define EMIT_MAX_RANGE 4000.0f
bool lights_emissive_light(World* w, Entity e, bool editor, GpuLight* g) {
    static int off = -1; // (REALENGINE_NO_EMISSIVE_LIGHTS=1: none, the measures' before)
    if (off < 0) off = getenv("REALENGINE_NO_EMISSIVE_LIGHTS") ? 1 : 0;
    if (off) return false;
    const CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!sm || !t || !sm->material || ecs_get(w, e, COMP_LIGHT)) return false;
    if ((editor && (t->flags & TRANSFORM_HIDDEN)) || (t->flags & (TRANSFORM_HIDDEN_GAME | TRANSFORM_NO_RENDER))) return false;
    const Material* m = mat_find_hash(sm->material);
    if (!m || !m->slot || m->emissive_light <= 0.0f || m->blend_mode == MAT_BLEND_TRANSLUCENT) return false;
    float nits[3], lum = 0.0f, ae[3];
    // (2026-11-24) the actor's own slot's emission (its dynamic instance's Emission Intensity a Blueprint set dims the light too)
    if (!mat_slot_emission(mat_slot_of(e, (Material*)m), ae)) memcpy(ae, m->avg_emis, sizeof ae);
    for (int k = 0; k < 3; k++) nits[k] = ae[k] * 100.0f * m->emissive_light;
    lum = 0.2126f * nits[0] + 0.7152f * nits[1] + 0.0722f * nits[2];
    if (!(lum > 0.0f)) return false;
    float mn[3], mx[3];
    render_mesh_bounds(sm->mesh, mn, mx);
    float rows[3][4], c[3], ext = 0.0f, smax = 0.0f;
    render_transform_rows(t, rows);
    for (int a = 0; a < 3; a++) smax = fmaxf(smax, fabsf(t->scale[a])), ext += (mx[a] - mn[a]) * (mx[a] - mn[a]);
    for (int r = 0; r < 3; r++)
        c[r] = rows[r][0] * (mn[0] + mx[0]) * 0.5f + rows[r][1] * (mn[1] + mx[1]) * 0.5f + rows[r][2] * (mn[2] + mx[2]) * 0.5f + rows[r][3];
    float radius = sqrtf(ext) * 0.5f * smax;
    float s2 = powf(fabsf(t->scale[0] * t->scale[1] * t->scale[2]), 2.0f / 3.0f); // (the area grows with the scale squared)
    float area_m2 = render_mesh_area(render_mesh_slot(sm->mesh)) * s2 * 1e-4f;
    float cd = lum * area_m2 * 0.25f;
    if (!(cd >= EMIT_MIN_CD) || !(radius > 0.0f)) return false;
    if (!g) return true;
    memset(g, 0, sizeof *g);
    memcpy(g->position, c, sizeof c);
    g->direction[2] = -1.0f, g->right[0] = 1.0f;
    for (int k = 0; k < 3; k++) g->radiance[k] = nits[k] * area_m2 * 0.25f;
    // a point light; casts by ray when there are rays; the GI's, the highlights' and the fog's scales at 1
    // (its shadows only when the material asks: a casting local light turns on the many lights' sampling and the
    // shadows' filter, ~1 ms in the materials scene at 1440p; unshadowed it costs ~0.3 ms there)
    g->type = 1u | (m->emissive_shadows && gi_available() ? GPU_LIGHT_CASTS : 0u) | 32u << 9 | 64u << 16 | 64u << 24;
    g->range = fminf(fmaxf(sqrtf(cd / EMIT_MIN_LUX) * 100.0f, radius * 2.0f + 50.0f), EMIT_MAX_RANGE);
    g->inv_range2 = 1.0f / (g->range * g->range);
    g->cos_outer = -1.0f, g->cos_inner = -1.0f;
    g->half_w = 0.0f, g->half_h = -radius; // (no Light Function; the shadow rays stop at its sphere)
    g->source_radius = radius;
    return true;
}

// none, LS_LOCAL or LS_GLOBAL (directional, sky); *dynamic: a local light that moves while playing
static int light_class(World* w, Entity e, bool editor, bool* dynamic) {
    *dynamic = false;
    if (e == ENTITY_NONE) return 0;
    const CLight* l = ecs_get(w, e, COMP_LIGHT);
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!l && lights_emissive_light(w, e, editor, NULL)) return *dynamic = true, LS_LOCAL; // (an emissive actor's: compared every frame)
    if (!l || !t || (editor && (t->flags & TRANSFORM_HIDDEN))) return 0;
    if (l->type == LIGHT_DIRECTIONAL || l->type == LIGHT_SKY) return LS_GLOBAL;
    *dynamic = (t->flags & TRANSFORM_MOBILITY_MASK) == MOBILITY_MOVABLE || (ecs_mask(w, e) & COMP_BIT(COMP_ROTATING)) ||
               terror_light_animated(l) || l->rim; // (Q13: a flickering light changes every frame, a Rim Light moves)
    return LS_LOCAL;
}

#define LEVEL_WEIGHTS 64
static float g_level_dw[LEVEL_WEIGHTS]; // the weight - 1 (zero-initialized: 1)
void lights_set_level_weight(uint32_t id, float w) {
    if (id < LEVEL_WEIGHTS) g_level_dw[id] = (w < 0.0f ? 0.0f : w) - 1.0f;
}
float lights_level_weight(World* w, Entity e) {
    const CStreamed* s = ecs_get(w, e, COMP_STREAMED);
    if (!s) {
        const CAttach* a = ecs_get(w, e, COMP_ATTACH); // (a Blueprint's light component: its actor's level)
        s = a && a->owner ? ecs_get(w, a->owner, COMP_STREAMED) : NULL;
    }
    return s && s->level < LEVEL_WEIGHTS ? 1.0f + g_level_dw[s->level] : 1.0f;
}

static bool g_lf_seen;
bool lights_function_seen(void) { return g_lf_seen; }
static bool g_toon_force_seen; // (Q11) a Force Toon light sent: its step is in the RE_TOON variants (render.c)
bool lights_toon_force_seen(void) { return g_toon_force_seen; }

bool lights_gpu(World* w, Entity e, bool editor, GpuLight* g) {
    if (e == ENTITY_NONE) return false;
    const CLight* l = ecs_get(w, e, COMP_LIGHT);
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!l) return lights_emissive_light(w, e, editor, g); // (2026-11-23) an emissive actor's light
    if (!l || !t || l->type == LIGHT_SKY || (editor && (t->flags & TRANSFORM_HIDDEN))) return false;
    float m[3][3];
    rotator_axes(t->rotation, m);
    memcpy(g->position, t->location, sizeof g->position);
    memcpy(g->direction, m[0], sizeof g->direction);
    memcpy(g->right, m[1], sizeof g->right);
    g->type = l->type == LIGHT_DIRECTIONAL ? 0u : l->type == LIGHT_POINT ? 1u : l->type == LIGHT_SPOT ? 2u : 3u;
    if (l->cast_shadows) g->type |= GPU_LIGHT_CASTS;
    // (Q13) its Lighting Channels in bits 4-6 (channel 0 flipped: 0 is channel 0 only), its beam's dust in bit 7
    g->type |= ((((l->light_channels ? l->light_channels : 1u) ^ 1u) & 7u) << 4) | (l->beam_dust ? 0x80u : 0u);
    if (l->ext && !l->affects_world) return false; // Affects World off: nothing to light
    // physical units: a directional light's lux, the others' candela (lux at 1 m); the temperature's tint
    lights_radiance(l, g->radiance);
    float lw = lights_level_weight(w, e); // (a Lighting Level's blend)
    if (lw <= 0.0f) return false;          // (its Lighting Level off: no light, no shadow; in the game too)
    lw *= terror_light_factor(l, e); // (Q13) its Light Flicker now (candle, faulty bulb, strobe, lightning, the tension's)
    if (lw != 1.0f)
        for (int k = 0; k < 3; k++) g->radiance[k] *= lw;
    // the GI's, the highlights' and the fog's scales packed in the type's free bits (common.glsl light_*_scale)
    float ind, spec, vol;
    lights_scales(l, &ind, &spec, &vol);
    g->type |= (uint32_t)fminf(fmaxf(vol * 32.0f + 0.5f, 0.0f), 127.0f) << 9 |
               (uint32_t)fminf(fmaxf(spec * 64.0f + 0.5f, 0.0f), 255.0f) << 16 |
               (uint32_t)fminf(fmaxf(ind * 64.0f + 0.5f, 0.0f), 255.0f) << 24;
    // (Q11) its toon mode in the kind's free bits (toon.glsl LIGHT_TOON_FORCE 0x04, LIGHT_TOON_NEVER 0x08)
    if (l->toon_mode == 1u) g->type |= 0x04u, g_toon_force_seen = true;
    else if (l->toon_mode == 2u) g->type |= 0x08u;
    g->range = fmaxf(l->attenuation_radius, 1.0f);
    g->inv_range2 = 1.0f / (g->range * g->range);
    float outer = fminf(fmaxf(l->outer_cone, 1.0f), 89.0f), inner = fminf(fmaxf(l->inner_cone, 0.0f), outer);
    g->cos_outer = cosf(outer * DEG2RAD);
    g->cos_inner = cosf(inner * DEG2RAD);
    g->half_w = l->source_width * 0.5f * t->scale[1];
    g->half_h = l->source_height * 0.5f * t->scale[2];
    // (2026-11-17, Q4) a Light Function (not on a rect light: its half sizes are its own): its material's slot + 1 in
    // half_w, a directional light's tile (cm) in half_h (mesh.frag light_fn; nothing else reads them off a rect light)
    if ((g->type & 3u) != 3u) { // (Q13: the kind alone; the type with its flags was never 3, a rect light lost its size)
        g->half_w = g->half_h = 0.0f;
        Material* lf = l->light_function ? mat_find_hash(l->light_function) : NULL;
        if (lf && lf->slot) {
            g->half_w = (float)(lf->slot + 1u);
            g->half_h = l->light_function_scale > 1.0f ? l->light_function_scale : 1000.0f;
            g_lf_seen = true;
        }
    }
    g->source_radius = l->source_radius;
    return true;
}

static bool grow_u32(uint32_t** a, uint32_t* cap, uint32_t need) {
    if (need <= *cap) return true;
    uint32_t c = *cap ? *cap * 2 : 64;
    while (c < need) c *= 2;
    uint32_t* n = realloc(*a, sizeof(uint32_t) * c); // the set grew: an edit, not a frame
    if (!n) return false;
    *a = n;
    *cap = c;
    return true;
}

static void globals_add(uint32_t s) {
    if (!grow_u32(&LS.globals, &LS.globals_cap, LS.nglobals + 1)) return;
    uint32_t j = LS.nglobals++;
    while (j > 0 && LS.globals[j - 1] > s) LS.globals[j] = LS.globals[j - 1], j--;
    LS.globals[j] = s;
}
static void globals_remove(uint32_t s) {
    for (uint32_t j = 0; j < LS.nglobals; j++)
        if (LS.globals[j] == s) {
            memmove(&LS.globals[j], &LS.globals[j + 1], sizeof(uint32_t) * (LS.nglobals - j - 1));
            LS.nglobals--;
            return;
        }
}
static void dyn_add(uint32_t s) {
    uint32_t c = LS.dyn_cap;
    if (!grow_u32(&LS.dyn, &c, LS.ndyn + 1)) return;
    if (c != LS.dyn_cap) {
        GpuLight* n = realloc(LS.dyn_copy, sizeof(GpuLight) * c);
        if (!n) return;
        LS.dyn_copy = n;
        LS.dyn_cap = c;
    }
    LS.dyn[LS.ndyn] = s;
    LS.dyn_copy[LS.ndyn++] = none_light(); // sent on the next compare
}
static void dyn_remove(uint32_t s) {
    for (uint32_t j = 0; j < LS.ndyn; j++)
        if (LS.dyn[j] == s) {
            LS.dyn[j] = LS.dyn[--LS.ndyn];
            LS.dyn_copy[j] = LS.dyn_copy[LS.ndyn];
            return;
        }
}

static void send(uint32_t frame_slot, uint32_t slot, const GpuLight* g) {
    GpuLight* st = LS.stage[frame_slot].mapped;
    st[LS.nregions] = *g;
    LS.regions[LS.nregions] = (VkBufferCopy){ (VkDeviceSize)LS.nregions * sizeof(GpuLight), (VkDeviceSize)slot * sizeof(GpuLight),
                                              sizeof(GpuLight) };
    LS.nregions++;
}

// A slot that may have changed class (created, destroyed, its type or mobility edited): the sets, then its light sent
// (the dynamic ones are sent by the compare)
static void update_slot(World* w, bool editor, uint32_t frame_slot, uint32_t s) {
    if (s >= LS.cap) return;
    bool dynamic;
    Entity e = ecs_entity_at(w, s);
    int c = light_class(w, e, editor, &dynamic);
    uint8_t was = LS.state[s];
    if ((was & LS_GLOBAL) && c != LS_GLOBAL) globals_remove(s);
    if (c == LS_GLOBAL && !(was & LS_GLOBAL)) globals_add(s);
    bool dyn = c == LS_LOCAL && dynamic;
    if ((was & LS_DYNAMIC) && !dyn) dyn_remove(s);
    if (dyn && !(was & LS_DYNAMIC)) dyn_add(s);
    LS.local_count += (c == LS_LOCAL ? 1u : 0u) - ((was & LS_LOCAL) ? 1u : 0u);
    GpuLight g;
    bool sent = false;
    if (c == LS_LOCAL && !dyn && lights_gpu(w, e, editor, &g)) send(frame_slot, s, &g), sent = true;
    else if (c != LS_LOCAL && (was & LS_LOCAL)) {
        g = none_light(); // gone (or now directional): its slot empty
        send(frame_slot, s, &g);
    }
    LS.state[s] = (uint8_t)(c | (dyn ? LS_DYNAMIC : 0) | (was & (LS_AREA | LS_CAST)));
    if (!dyn) area_mark(s, sent ? &g : NULL); // (a Movable one: marked by its compare in lights_prepare)
}

bool lights_init(struct VkCtx* vk, VkPipelineLayout layout, const uint32_t* grid_code, size_t grid_bytes) {
    memset(&LS, 0, sizeof LS);
    LS.sel_slot = UINT32_MAX;
    LS.grid_pipe = gpu_compute_pipeline(vk, layout, grid_code, grid_bytes);
    if (!LS.grid_pipe) return false;
    VkBufferUsageFlags st = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (!gpu_buf_create(vk, &LS.cells, (VkDeviceSize)LG_CELLS * 8, st, false) ||
        !gpu_buf_create(vk, &LS.cursor, (VkDeviceSize)LG_CELLS * 4 + 16, st | VK_BUFFER_USAGE_TRANSFER_SRC_BIT, false) ||
        !gpu_buf_create(vk, &LS.items, (VkDeviceSize)LG_ITEMS_CAP * 4, st, false))
        return false;
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++)
        if (!gpu_buf_create(vk, &LS.read[f], 16, VK_BUFFER_USAGE_TRANSFER_DST_BIT, true)) return false;
    return true;
}

void lights_shutdown(struct VkCtx* vk) {
    if (LS.grid_pipe) vkDestroyPipeline(vk->device, LS.grid_pipe, NULL);
    GpuBuf* bufs[] = { &LS.lights, &LS.cells, &LS.cursor, &LS.items };
    for (int i = 0; i < 4; i++) gpu_buf_destroy(vk, bufs[i]);
    for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) gpu_buf_destroy(vk, &LS.stage[f]), gpu_buf_destroy(vk, &LS.read[f]);
    free(LS.state);
    free(LS.dyn);
    free(LS.dyn_copy);
    free(LS.globals);
    free(LS.cast_slots), free(LS.cast_at), free(LS.cast_gl);
    memset(&LS, 0, sizeof LS);
}

bool lights_reserve(struct VkCtx* vk, uint32_t cap) {
    if (LS.lights.buffer && cap <= LS.cap) return true;
    vkDeviceWaitIdle(vk->device);
    gpu_buf_destroy(vk, &LS.lights);
    if (!gpu_buf_create(vk, &LS.lights, (VkDeviceSize)cap * sizeof(GpuLight),
                        VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false))
        return false;
    uint8_t* s = realloc(LS.state, cap); // a resize, not a frame
    if (!s) return false;
    LS.state = s;
    uint32_t* ca = realloc(LS.cast_at, sizeof(uint32_t) * cap);
    if (!ca) return false;
    LS.cast_at = ca;
    memset(LS.cast_at, 0xFF, sizeof(uint32_t) * cap), LS.ncast = 0; // (a rebuild follows)
    LS.cap = cap;
    LS.grid_valid = false;
    return true;
}

bool lights_rebuild(struct VkCtx* vk, World* w, bool editor) {
    memset(LS.state, 0, LS.cap);
    if (LS.cast_at) memset(LS.cast_at, 0xFF, sizeof(uint32_t) * LS.cap);
    LS.ncast = 0;
    LS.ndyn = LS.nglobals = LS.local_count = LS.area_count = LS.cast_count = 0;
    LS.sel_slot = UINT32_MAX;
    LS.grid_valid = false;
    GpuBuf staging = { 0 };
    VkDeviceSize bytes = (VkDeviceSize)LS.cap * sizeof(GpuLight);
    if (!gpu_buf_create(vk, &staging, bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) return false;
    GpuLight* st = staging.mapped;
    memset(st, 0xFF, (size_t)bytes);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_LIGHT) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need) continue;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            uint32_t s = entity_index(ents[i]);
            if (s >= LS.cap) continue;
            bool dynamic;
            int c = light_class(w, ents[i], editor, &dynamic);
            if (c == LS_GLOBAL) globals_add(s);
            if (c != LS_LOCAL) {
                LS.state[s] = (uint8_t)c;
                continue;
            }
            LS.local_count++;
            LS.state[s] = (uint8_t)(LS_LOCAL | (dynamic ? LS_DYNAMIC : 0));
            if (dynamic) dyn_add(s);
            else if (lights_gpu(w, ents[i], editor, &st[s])) area_mark(s, &st[s]);
        }
    }
    // (2026-11-23) the emissive actors' lights: in the compared set, sent by the next frame's compare
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || (a->mask & COMP_BIT(COMP_LIGHT))) continue;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            uint32_t s = entity_index(ents[i]);
            if (s >= LS.cap || !lights_emissive_light(w, ents[i], editor, NULL)) continue;
            LS.local_count++;
            LS.state[s] = (uint8_t)(LS_LOCAL | LS_DYNAMIC);
            dyn_add(s);
        }
    }
    VkCommandBuffer cmd = gpu_begin_once(vk);
    VkBufferCopy region = { 0, 0, bytes };
    vkCmdCopyBuffer(cmd, staging.buffer, LS.lights.buffer, 1, &region);
    gpu_end_once(vk, cmd);
    gpu_buf_destroy(vk, &staging);
    return true;
}

bool lights_prepare(struct VkCtx* vk, World* w, bool editor, uint32_t frame_slot, const uint32_t* hot, uint32_t nhot,
                    Entity selected) {
    LS.nregions = 0;
    uint32_t total = LS.ndyn + nhot * 2 + 2; // a hot slot may join the dynamic set: sent by the compare too
    if (total > LS.stage_cap) {
        uint32_t c = LS.stage_cap ? LS.stage_cap : 256;
        while (c < total) c *= 2;
        vkDeviceWaitIdle(vk->device);
        for (int f = 0; f < RE_FRAMES_IN_FLIGHT; f++) {
            gpu_buf_destroy(vk, &LS.stage[f]);
            if (!gpu_buf_create(vk, &LS.stage[f], (VkDeviceSize)c * sizeof(GpuLight), VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true))
                return false;
        }
        LS.stage_cap = c;
    }
    LS.regions = RE_FRAME_ALLOC(VkBufferCopy, total);
    if (!LS.regions) return false;
    for (uint32_t i = 0; i < nhot; i++) update_slot(w, editor, frame_slot, hot[i]);
    // the selection: Details edits it in place (no change log): its class checked, its light sent when it differs
    if (selected != ENTITY_NONE && entity_index(selected) < LS.cap) {
        uint32_t s = entity_index(selected);
        bool dynamic;
        int c = light_class(w, selected, editor, &dynamic);
        uint8_t now = (uint8_t)(c | (c == LS_LOCAL && dynamic ? LS_DYNAMIC : 0));
        GpuLight g;
        if (now != LS.state[s]) {
            update_slot(w, editor, frame_slot, s);
            LS.sel_slot = UINT32_MAX;
        } else if (c == LS_LOCAL && !dynamic && lights_gpu(w, selected, editor, &g) &&
                   (s != LS.sel_slot || memcmp(&g, &LS.sel_copy, sizeof g) != 0)) {
            send(frame_slot, s, &g);
            area_mark(s, &g);
            LS.sel_slot = s;
            LS.sel_copy = g;
        }
    }
    // the Movable lights: sent when they differ from their last copy
    for (uint32_t i = 0; i < LS.ndyn; i++) {
        GpuLight g;
        bool ok = lights_gpu(w, ecs_entity_at(w, LS.dyn[i]), editor, &g);
        if (!ok) g = none_light();
        if (memcmp(&g, &LS.dyn_copy[i], sizeof g) != 0) {
            LS.dyn_copy[i] = g;
            send(frame_slot, LS.dyn[i], &g);
            area_mark(LS.dyn[i], ok ? &g : NULL);
        }
    }
    return true;
}

void lights_fill_frame(GpuFrame* fr, const float camera[3], uint32_t slots, uint32_t shadow_budget) {
    LS.slots = slots < LS.cap ? slots : LS.cap;
    fr->light_buf = LS.lights.addr;
    fr->lg_cells = LS.cells.addr;
    fr->lg_items = LS.items.addr;
    fr->lg_cursor = LS.cursor.addr;
    for (int L = 0; L < LG_LEVELS; L++) {
        float cs = LG_CELL0_CM * (float)(1 << (2 * L));
        for (int k = 0; k < 3; k++) {
            LS.origin[L][k] = (int32_t)floorf(camera[k] / cs) - (k < 2 ? 32 : 8); // the camera's cell in the middle
            fr->lg_origin[L][k] = LS.origin[L][k];
        }
        fr->lg_origin[L][3] = 0;
    }
    fr->lg_params[0] = LG_CELL0_CM;
    fr->lg_params[1] = LG_LOD;
    fr->lg_params[2] = 0.0f;
    fr->lg_params[3] = (float)LS.slots;
    fr->local_lights = LS.local_count;
    fr->lg_cap = LG_ITEMS_CAP;
    fr->shadow_budget = shadow_budget;
    fr->lg_pad = 0;
}

// (2026-11-17) a slot sent twice in a frame (touched twice: a Lighting Level's weight, then every level's) leaves only
// its last copy: overlapping regions in one vkCmdCopyBuffer are a write-after-write hazard (VUID-vkCmdCopyBuffer-pRegions)
static int region_cmp(const void* a, const void* b) {
    const VkBufferCopy *x = a, *y = b;
    if (x->dstOffset != y->dstOffset) return x->dstOffset < y->dstOffset ? -1 : 1;
    return x->srcOffset > y->srcOffset ? -1 : x->srcOffset < y->srcOffset; // (the later one first)
}
static void regions_dedupe(void) {
    if (LS.nregions < 2) return;
    qsort(LS.regions, LS.nregions, sizeof(VkBufferCopy), region_cmp);
    uint32_t n = 1;
    for (uint32_t i = 1; i < LS.nregions; i++)
        if (LS.regions[i].dstOffset != LS.regions[n - 1].dstOffset) LS.regions[n++] = LS.regions[i];
    LS.nregions = n;
}

void lights_record(VkCommandBuffer cmd, uint32_t frame_slot, VkPipelineLayout layout, VkDescriptorSet set, Push push) {
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, COPY = VK_PIPELINE_STAGE_2_COPY_BIT;
    VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    bool changed = LS.nregions > 0;
    if (changed) {
        regions_dedupe();
        vkCmdCopyBuffer(cmd, LS.stage[frame_slot].buffer, LS.lights.buffer, LS.nregions, LS.regions);
        gpu_barrier(cmd, COPY, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    }
    // the grid only when a light changed or the camera crossed a cell of level 0 (the levels move with it)
    if (!LS.local_count) return;
    if (LS.grid_valid && !changed && LS.built_slots == LS.slots && !memcmp(LS.origin, LS.built_origin, sizeof LS.origin))
        return;
    gpu_barrier(cmd, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT, COPY,
                VK_ACCESS_2_TRANSFER_WRITE_BIT); // the last frame's readers of the grid
    vkCmdFillBuffer(cmd, LS.cells.buffer, 0, VK_WHOLE_SIZE, 0);
    vkCmdFillBuffer(cmd, LS.cursor.buffer, 0, VK_WHOLE_SIZE, 0);
    gpu_barrier(cmd, COPY, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, LS.grid_pipe);
    uint32_t groups[3] = { (LS.slots + 63) / 64, LG_CELLS / 64, (LS.slots + 63) / 64 };
    for (uint32_t p = 0; p < 3; p++) {
        push.pass = p;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof push, &push);
        if (groups[p]) vkCmdDispatch(cmd, groups[p], 1, 1);
        gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, CS | COPY, RW | VK_ACCESS_2_TRANSFER_READ_BIT);
    }
    VkBufferCopy rc = { (VkDeviceSize)LG_CELLS * 4, 0, 4 };
    vkCmdCopyBuffer(cmd, LS.cursor.buffer, LS.read[frame_slot].buffer, 1, &rc);
    gpu_barrier(cmd, CS, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
    memcpy(LS.built_origin, LS.origin, sizeof LS.origin);
    LS.built_slots = LS.slots;
    LS.grid_valid = true;
}

const uint32_t* lights_globals(uint32_t* n) {
    *n = LS.nglobals;
    return LS.globals;
}

uint32_t lights_local_count(void) { return LS.local_count; }
uint32_t lights_area_count(void) { return LS.area_count; }
uint32_t lights_cast_count(void) { return LS.cast_count; }
uint32_t lights_cast_list(const uint32_t** slots, const GpuLight** gl) {
    *slots = LS.cast_slots;
    *gl = LS.cast_gl;
    return LS.ncast;
}

uint32_t lights_grid_items(uint32_t frame_slot, uint32_t* capacity) {
    *capacity = LG_ITEMS_CAP;
    const uint32_t* r = LS.read[frame_slot].mapped;
    return r ? r[0] : 0;
}

// --- Unreal's light options (components.h CLight) ---

// A black body's color at k kelvin as linear sRGB, 6500 K white, luminance 1: the CIE 1931 Planckian locus (Kang et al.
// 2002 cubic fits of x, y; 1667-25000 K), xyY -> XYZ -> sRGB, divided by D65's
static void kelvin_rgb(float k, float rgb[3]) {
    k = fminf(fmaxf(k, 1667.0f), 25000.0f);
    float t = 1000.0f / k, t2 = t * t, t3 = t2 * t;
    float x = k <= 4000.0f ? -0.2661239f * t3 - 0.2343589f * t2 + 0.8776956f * t + 0.179910f
                           : -3.0258469f * t3 + 2.1070379f * t2 + 0.2226347f * t + 0.240390f;
    float x2 = x * x, x3 = x2 * x;
    float y = k <= 2222.0f   ? -1.1063814f * x3 - 1.34811020f * x2 + 2.18555832f * x - 0.20219683f
              : k <= 4000.0f ? -0.9549476f * x3 - 1.37418593f * x2 + 2.09137015f * x - 0.16748867f
                             : 3.0817580f * x3 - 5.87338670f * x2 + 3.75112997f * x - 0.37001483f;
    float X = x / y, Z = (1.0f - x - y) / y; // Y = 1
    float r = 3.2404542f * X - 1.5371385f - 0.4985314f * Z;
    float g = -0.9692660f * X + 1.8760108f + 0.0415560f * Z;
    float b = 0.0556434f * X - 0.2040259f + 1.0572252f * Z;
    // D65 (x 0.3127, y 0.3290) is (1, 1, 1) in sRGB at Y = 1: a 6500 K light is close to white
    r = fmaxf(r, 0.0f), g = fmaxf(g, 0.0f), b = fmaxf(b, 0.0f);
    float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
    rgb[0] = r / lum, rgb[1] = g / lum, rgb[2] = b / lum;
}

void lights_scales(const CLight* l, float* indirect, float* specular, float* volumetric) {
    bool e = l->ext != 0;
    *indirect = e ? l->indirect_intensity : 1.0f;
    *specular = e ? l->specular_scale : 1.0f;
    *volumetric = e ? l->volumetric_scattering : 1.0f;
}

void lights_radiance(const CLight* l, float out[3]) {
    float k[3] = { 1.0f, 1.0f, 1.0f };
    if (l->ext && l->use_temperature) kelvin_rgb(l->temperature, k);
    float on = l->ext && !l->affects_world ? 0.0f : 1.0f;
    for (int i = 0; i < 3; i++) out[i] = l->color[i] * k[i] * l->intensity * on;
}

// The solid angle the units spread a light over (lumens = cd x it): a point light the sphere, a spot its outer cone (as
// Unreal), a rect light pi (a one-sided Lambertian emitter); Blender's Watts: lumens at 683 lm/W
static float light_steradians(const CLight* l) {
    if (l->type == LIGHT_SPOT) {
        float half = fminf(fmaxf(l->outer_cone, 1.0f), 89.0f) * DEG2RAD;
        return 6.2831853f * (1.0f - cosf(half));
    }
    return l->type == LIGHT_RECT ? 3.14159265f : 12.566371f;
}

float lights_to_units(const CLight* l, LightUnits u) {
    float v = l->intensity;
    if (l->type == LIGHT_DIRECTIONAL) return u == LIGHT_UNITS_WATTS ? v / 683.0f : v; // lux or W/m^2
    switch (u) {
    case LIGHT_UNITS_LUMENS: return v * light_steradians(l);
    case LIGHT_UNITS_EV: return v > 0.0f ? log2f(v / 0.125f) : -10.0f; // Unreal: cd = 2^EV / 8
    case LIGHT_UNITS_WATTS: return v * (l->type == LIGHT_SPOT ? 12.566371f : light_steradians(l)) / 683.0f; // Blender's spot: a masked point
    default: return v;
    }
}

void lights_from_units(CLight* l, LightUnits u, float value) {
    if (l->type == LIGHT_DIRECTIONAL) {
        l->intensity = u == LIGHT_UNITS_WATTS ? value * 683.0f : value;
        return;
    }
    switch (u) {
    case LIGHT_UNITS_LUMENS: l->intensity = value / light_steradians(l); break;
    case LIGHT_UNITS_EV: l->intensity = 0.125f * exp2f(value); break;
    case LIGHT_UNITS_WATTS: l->intensity = value * 683.0f / (l->type == LIGHT_SPOT ? 12.566371f : light_steradians(l)); break;
    default: l->intensity = value; break;
    }
    if (l->intensity < 0.0f) l->intensity = 0.0f;
}
