#include "fire.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "async.h"
#include "components.h"
#include "i18n.h"
#include "render.h"
#include "vkctx.h"
#include "volume.h"

static const uint32_t s_fire_sim_comp[] =
#include "fire_sim.comp.inc"
    ;
static const uint32_t s_fire_render_comp[] =
#include "fire_render.comp.inc"
    ;

#define FIRE_MAX 16
#define FIRE_PASSES 7

// fire.glsl FireInfo (scalar)
typedef struct FireGpu {
    float rows[3][4], inv[3][4];
    uint32_t dims[4];
    float cell, dt, time, cur;
    float buoyancy, vorticity, cooling, dissipation;
    float burn, heat, smoke, fuel;
    float emitter[4], wind[4], look[4], sun[4];
} FireGpu;
_Static_assert(sizeof(FireGpu) == 224, "fire.glsl FireInfo");

typedef struct FireState {
    Entity e;
    uint32_t nx, ny, nz, base;
    float next_read, time;
    uint32_t iterations;
    bool used, run;
    // (A9.5a) a Heterogeneous Volume: its clip played instead of a gas stepped; its frame into a staging buffer (one a
    // frame in flight), copied before the sun's pass when it changes
    bool playback, need_up;
    VolumeClip clip;
    GpuBuf stage[RE_FRAMES_IN_FLIGHT];
    uint32_t frame_up;
    uint32_t loading_key; // (2026-11-25) the clip a worker reads now (0: none): drawn when it comes (vol_done)
} FireState;

static struct {
    struct VkCtx* vk;
    VkPipelineLayout layout;
    VkPipeline sim, render;
    GpuBuf fields, info[RE_FRAMES_IN_FLIGHT];
    VkDeviceSize cap;
    bool clear;          // the fields made again: zeroed before the first step
    FireState s[FIRE_MAX];
    uint32_t n, slot;
    FireStats stats;
} F;

const char* const g_fire_preset_names[] = { "Custom", "Campfire", "Torch", "Bonfire", "Candle" };

void fire_apply_preset(CFire* c, int preset) {
    c->preset = (uint32_t)preset;
    switch (preset) {
    case 1: c->width = 120.0f, c->height = 200.0f, c->emitter_radius = 30.0f, c->fuel = 1.0f, c->light_intensity = 4000.0f, c->resolution = 40; break; // campfire
    case 2: c->width = 40.0f, c->height = 90.0f, c->emitter_radius = 6.0f, c->fuel = 1.0f, c->light_intensity = 1500.0f, c->resolution = 32; break;   // torch
    case 3: c->width = 300.0f, c->height = 600.0f, c->emitter_radius = 90.0f, c->fuel = 1.3f, c->light_intensity = 20000.0f, c->resolution = 48; break; // bonfire
    case 4: c->width = 8.0f, c->height = 20.0f, c->emitter_radius = 0.8f, c->fuel = 0.8f, c->light_intensity = 60.0f, c->resolution = 24; break;     // candle
    default: break;
    }
}

const FireStats* fire_stats(void) { return &F.stats; }
bool fire_any(void) { return F.stats.fires > 0; }

bool fire_init(struct VkCtx* vk, VkPipelineLayout layout) {
    memset(&F, 0, sizeof F);
    F.vk = vk, F.layout = layout;
    F.sim = gpu_compute_pipeline(vk, layout, s_fire_sim_comp, sizeof s_fire_sim_comp);
    F.render = gpu_compute_pipeline(vk, layout, s_fire_render_comp, sizeof s_fire_render_comp);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++)
        if (!gpu_buf_create(vk, &F.info[i], sizeof(FireGpu) * FIRE_MAX, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
    return F.sim && F.render;
}

static void state_free(struct VkCtx* vk, FireState* s) {
    volume_free(&s->clip);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &s->stage[i]);
}
void fire_shutdown(struct VkCtx* vk) {
    for (uint32_t i = 0; i < F.n; i++) state_free(vk, &F.s[i]);
    if (F.sim) vkDestroyPipeline(vk->device, F.sim, NULL);
    if (F.render) vkDestroyPipeline(vk->device, F.render, NULL);
    gpu_buf_destroy(vk, &F.fields);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &F.info[i]);
    memset(&F, 0, sizeof F);
}

static void inverse_rows(const float m[3][4], float o[3][4]) {
    float a = m[0][0], b = m[0][1], c = m[0][2], d = m[1][0], e = m[1][1], f = m[1][2], g = m[2][0], h = m[2][1], k = m[2][2];
    float A = e * k - f * h, B = -(d * k - f * g), C = d * h - e * g, det = a * A + b * B + c * C;
    float id = fabsf(det) > 1e-20f ? 1.0f / det : 0.0f;
    float inv[3][3] = { { A * id, -(b * k - c * h) * id, (b * f - c * e) * id },
                        { B * id, (a * k - c * g) * id, -(a * f - c * d) * id },
                        { C * id, -(a * h - b * g) * id, (a * e - b * d) * id } };
    for (int i = 0; i < 3; i++) {
        for (int j = 0; j < 3; j++) o[i][j] = inv[i][j];
        o[i][3] = -(inv[i][0] * m[0][3] + inv[i][1] * m[1][3] + inv[i][2] * m[2][3]);
    }
}

// a smooth 1D noise (the light's flicker)
static float n1(float t) {
    float i = floorf(t), f = t - i;
    float a = sinf(i * 12.9898f) * 43758.5453f, b = sinf((i + 1.0f) * 12.9898f) * 43758.5453f;
    a -= floorf(a), b -= floorf(b);
    f = f * f * (3.0f - 2.0f * f);
    return a + (b - a) * f;
}

// (2026-11-25, bug-fixer) A Heterogeneous Volume's clip read on a worker: the files (24 .vdb for the example) read and
// resampled into its grid took the editor's frame (1096 ms the frame it was placed, the UI sweep). The volume shows
// when its clip comes; nothing else waits (a --bench waits for it: async_idle)
typedef struct VolLoad {
    Entity e;
    uint32_t key;
    CVolume v; // (a copy: the actor may change or go meanwhile)
    VolumeClip clip;
    bool ok;
} VolLoad;
static const char* vol_label(void) { return TR("Loading a Heterogeneous Volume"); } // (fire_fill's "tr" is a transform)
static void vol_work(AsyncTask* t, void* arg) {
    (void)t;
    VolLoad* L = arg;
    L->ok = volume_load(&L->v, &L->clip);
}
static void vol_done(AsyncTask* t, void* arg, bool ok) { // (the main thread, at a frame's start)
    (void)t;
    VolLoad* L = arg;
    FireState* s = NULL;
    for (uint32_t i = 0; i < F.n && !s; i++)
        if (F.s[i].e == L->e && F.s[i].loading_key == L->key) s = &F.s[i];
    if (s && F.vk) {
        s->loading_key = 0;
        vkDeviceWaitIdle(F.vk->device); // (its staging buffers may be read)
        for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(F.vk, &s->stage[i]);
        volume_free(&s->clip);
        if (ok && L->ok) s->clip = L->clip, L->clip.data = NULL;
        s->clip.key = L->key; // (tried once for these options, read or not)
        s->frame_up = ~0u, s->time = 0.0f;
    }
    volume_free(&L->clip); // (the actor gone or changed meanwhile: dropped)
    free(L);
}

void fire_fill(World* w, float dt, const float sun_dir[3], uint32_t slot) {
    F.slot = slot % RE_FRAMES_IN_FLIGHT;
    F.stats.fires = F.stats.cells = 0;
    if (!F.vk || !F.sim || getenv("REALENGINE_NO_FIRE")) return;
    if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f; // (a long frame: the gas steps at most 33 ms)
    for (uint32_t i = 0; i < F.n; i++) F.s[i].used = false;
    struct { uint32_t st; float rows[3][4]; CFire* c; CLight* l; } todo[FIRE_MAX];
    uint32_t nt = 0;
    bool relayout = false;
    for (uint32_t ai = 0; ai < w->archetype_count && nt < FIRE_MAX; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_FIRE)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        CFire* fc = ecs_arch_column(a, COMP_FIRE);
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        CLight* lc = (a->mask & COMP_BIT(COMP_LIGHT)) ? ecs_arch_column(a, COMP_LIGHT) : NULL;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t r = 0; r < a->count && nt < FIRE_MAX; r++) {
            CFire* c = &fc[r];
            if (!c->enabled) continue;
            uint32_t res = c->resolution < 12 ? 12 : c->resolution > 96 ? 96 : c->resolution;
            float wdt = c->width > 1.0f ? c->width : 1.0f, hgt = c->height > 1.0f ? c->height : 1.0f;
            uint32_t nz = (uint32_t)(res * hgt / wdt + 0.5f);
            nz = nz < 8 ? 8 : nz > 192 ? 192 : nz;
            FireState* s = NULL;
            for (uint32_t i = 0; i < F.n && !s; i++)
                if (F.s[i].e == ents[r]) s = &F.s[i];
            if (!s && F.n < FIRE_MAX) s = &F.s[F.n++], memset(s, 0, sizeof *s), s->e = ents[r], relayout = true;
            if (!s) continue;
            if (s->nx != res || s->ny != res || s->nz != nz) s->nx = s->ny = res, s->nz = nz, relayout = true;
            s->used = true;
            todo[nt].st = (uint32_t)(s - F.s), todo[nt].c = c, todo[nt].l = lc ? &lc[r] : NULL;
            render_transform_rows(&tr[r], todo[nt].rows);
            nt++;
        }
    }
    // (A9.5a) the Heterogeneous Volumes: their clips (loaded when their file or options change), the box the files cover
    struct { uint32_t st; float rows[3][4]; const CVolume* v; } vtodo[FIRE_MAX];
    uint32_t nv = 0;
    for (uint32_t ai = 0; ai < w->archetype_count && nt + nv < FIRE_MAX; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_VOLUME)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CVolume* vc = ecs_arch_column(a, COMP_VOLUME);
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t r = 0; r < a->count && nt + nv < FIRE_MAX; r++) {
            if (!vc[r].path[0] || (tr[r].flags & TRANSFORM_HIDDEN_GAME)) continue;
            FireState* s = NULL;
            for (uint32_t i = 0; i < F.n && !s; i++)
                if (F.s[i].e == ents[r]) s = &F.s[i];
            if (!s && F.n < FIRE_MAX) s = &F.s[F.n++], memset(s, 0, sizeof *s), s->e = ents[r], s->playback = true, relayout = true;
            if (!s) continue;
            uint32_t vkey = volume_key(&vc[r]);
            if (s->clip.key != vkey && s->loading_key != vkey) { // (read on a worker: vol_done puts it in place)
                VolLoad* L = calloc(1, sizeof *L);
                if (L) {
                    L->e = ents[r], L->key = vkey, L->v = vc[r];
                    s->loading_key = vkey;
                    // (no workers: it runs here and vol_done puts it in place at once, as before)
                    async_run(vol_label(), vol_work, vol_done, L, ASYNC_QUIET);
                }
            }
            if (!s->clip.data) { s->used = true; continue; }
            if (s->nx != s->clip.nx || s->ny != s->clip.ny || s->nz != s->clip.nz) s->nx = s->clip.nx, s->ny = s->clip.ny, s->nz = s->clip.nz, relayout = true;
            s->used = true;
            vtodo[nv].st = (uint32_t)(s - F.s), vtodo[nv].v = &vc[r];
            // the actor's place, then the box: its middle (x, y) and a fifth of its height over its floor (fire.glsl's frame)
            float rows[3][4], H = (float)s->nz * s->clip.cell;
            render_transform_rows(&tr[r], rows);
            float off[3] = { s->clip.center[0], s->clip.center[1], s->clip.bottom + 0.2f * H };
            for (int i = 0; i < 3; i++) {
                for (int j = 0; j < 3; j++) vtodo[nv].rows[i][j] = rows[i][j];
                vtodo[nv].rows[i][3] = rows[i][3] + rows[i][0] * off[0] + rows[i][1] * off[1] + rows[i][2] * off[2];
            }
            nv++;
        }
    }
    for (uint32_t i = 0; i < F.n;) { // (the fires gone)
        if (!F.s[i].used) {
            state_free(F.vk, &F.s[i]);
            F.s[i] = F.s[--F.n];
            for (uint32_t k = 0; k < nv; k++)
                if (vtodo[k].st == F.n) vtodo[k].st = i;
            for (uint32_t k = 0; k < nt; k++)
                if (todo[k].st == F.n) todo[k].st = i;
            relayout = true;
        } else
            i++;
    }
    if (relayout) { // every fire's room again (all start empty: a rare event, the GPU idled)
        VkDeviceSize need = 0;
        for (uint32_t i = 0; i < F.n; i++) F.s[i].base = (uint32_t)(need / 16u), need += (VkDeviceSize)F.s[i].nx * F.s[i].ny * F.s[i].nz * 6u * 16u;
        vkDeviceWaitIdle(F.vk->device);
        if (need > F.cap) {
            gpu_buf_destroy(F.vk, &F.fields);
            F.cap = 0;
            if (!gpu_buf_create(F.vk, &F.fields, need, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false)) {
                printf("[RealEngine] aviso: o fogo nao conseguiu %.1f MB\n", (double)need / 1048576.0);
                F.n = 0;
                return;
            }
            F.cap = need;
        }
        F.clear = F.n > 0;
        for (uint32_t i = 0; i < F.n; i++) F.s[i].next_read = 0.0f, F.s[i].time = 0.0f, F.s[i].frame_up = ~0u;
        if (F.n) printf("[RealEngine] Fire: %u fire(s), %.1f MB of grid\n", F.n, (double)need / 1048576.0);
    }
    FireGpu* out = F.info[F.slot].mapped;
    for (uint32_t k = 0; k < nt; k++) {
        FireState* s = &F.s[todo[k].st];
        CFire* c = todo[k].c;
        FireGpu* g = &out[k];
        memset(g, 0, sizeof *g);
        memcpy(g->rows, todo[k].rows, sizeof g->rows);
        inverse_rows(todo[k].rows, g->inv);
        g->dims[0] = s->nx, g->dims[1] = s->ny, g->dims[2] = s->nz, g->dims[3] = s->base;
        g->cell = (c->width > 1.0f ? c->width : 1.0f) / (float)s->nx;
        s->run = dt > 0.0f;
        g->dt = dt;
        g->time = s->time;
        s->time += dt;
        // (fire.glsl: the step reads cur and writes the other; drawn from the other. Held: the last written drawn)
        g->cur = s->run ? s->next_read : 1.0f - s->next_read;
        if (s->run) s->next_read = 1.0f - s->next_read;
        float cell_m = g->cell * 0.01f; // (the forces scaled to the grid: cells per second)
        g->buoyancy = c->buoyancy / cell_m, g->vorticity = c->vorticity, g->cooling = c->cooling, g->dissipation = c->dissipation;
        g->burn = c->burn, g->heat = c->heat, g->smoke = c->smoke, g->fuel = c->fuel;
        g->emitter[0] = s->nx * 0.5f, g->emitter[1] = s->ny * 0.5f, g->emitter[2] = 0.5f, g->emitter[3] = c->emitter_radius / g->cell;
        // the wind: none of its own yet (Falta: the weather's); the emitter's turbulence in cells/s
        g->wind[3] = c->turbulence / g->cell;
        g->look[0] = c->brightness, g->look[1] = c->smoke_density, g->look[2] = c->smoke_albedo;
        if (sun_dir) {
            float d[3];
            for (int i = 0; i < 3; i++) d[i] = g->inv[i][0] * sun_dir[0] + g->inv[i][1] * sun_dir[1] + g->inv[i][2] * sun_dir[2];
            float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (l > 1e-6f) g->sun[0] = d[0] / l, g->sun[1] = d[1] / l, g->sun[2] = d[2] / l, g->sun[3] = 1.0f;
        }
        s->iterations = c->iterations < 4 ? 4 : c->iterations > 80 ? 80 : (c->iterations + 1u) & ~1u;
        // its light flickers with it (the actor's Light: a point light over the fuel)
        if (todo[k].l) {
            CLight* l = todo[k].l;
            float fl = c->flicker < 0.0f ? 0.0f : c->flicker > 1.0f ? 1.0f : c->flicker;
            float nn = 0.6f * n1(s->time * 7.0f) + 0.4f * n1(s->time * 17.0f + 3.0f);
            l->intensity = c->light_intensity * (1.0f - fl * 0.5f + fl * nn * 0.5f) * (c->fuel > 0.0f ? 1.0f : 0.0f);
            l->use_temperature = 1, l->temperature = 1800.0f;
            if (l->attenuation_radius <= 0.0f || c->light_radius > 0.0f) l->attenuation_radius = c->light_radius > 0.0f ? c->light_radius : 1000.0f;
        }
        F.stats.fires++, F.stats.cells += s->nx * s->ny * s->nz;
    }
    for (uint32_t k = 0; k < nv && F.stats.fires < FIRE_MAX; k++) { // (A9.5a) the volumes: their frame, their look
        FireState* s = &F.s[vtodo[k].st];
        const CVolume* v = vtodo[k].v;
        FireGpu* g = &out[F.stats.fires];
        memset(g, 0, sizeof *g);
        memcpy(g->rows, vtodo[k].rows, sizeof g->rows);
        inverse_rows(vtodo[k].rows, g->inv);
        g->dims[0] = s->nx, g->dims[1] = s->ny, g->dims[2] = s->nz, g->dims[3] = s->base;
        g->cell = s->clip.cell, g->dt = 0.0f, g->cur = 1.0f; // (drawn from the scalars A: base + 2N)
        s->run = false;
        s->time += dt > 0.0f ? dt : 0.0f;
        uint32_t fr = (uint32_t)(s->time * (v->fps > 0.1f ? v->fps : 24.0f));
        fr = v->loop ? fr % s->clip.frames : (fr >= s->clip.frames ? s->clip.frames - 1 : fr);
        g->look[0] = v->brightness, g->look[1] = v->density * 0.01f, g->look[2] = v->smoke_albedo;
        if (sun_dir) {
            float d[3];
            for (int i = 0; i < 3; i++) d[i] = g->inv[i][0] * sun_dir[0] + g->inv[i][1] * sun_dir[1] + g->inv[i][2] * sun_dir[2];
            float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (l > 1e-6f) g->sun[0] = d[0] / l, g->sun[1] = d[1] / l, g->sun[2] = d[2] / l, g->sun[3] = 1.0f;
        }
        if (fr != s->frame_up) { // its frame into this slot's staging buffer: temperature, density, -, the sun (pass 6)
            size_t cells = (size_t)s->nx * s->ny * s->nz;
            GpuBuf* st = &s->stage[F.slot];
            if (!st->buffer && !gpu_buf_create(F.vk, st, cells * 16u, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) continue;
            const uint16_t* src = s->clip.data + 2 * cells * fr;
            float* dst = st->mapped;
            for (size_t i = 0; i < cells; i++) {
                for (int c = 0; c < 2; c++) {
                    uint16_t h = src[2 * i + c];
                    uint32_t e = (h >> 10) & 31u, m = h & 1023u, x = e ? ((e + 112u) << 23 | m << 13) : 0u;
                    memcpy(&dst[4 * i + c], &x, 4);
                }
                dst[4 * i + 2] = 0.0f, dst[4 * i + 3] = 1.0f;
            }
            s->frame_up = fr, s->need_up = true;
        }
        F.stats.fires++, F.stats.cells += s->nx * s->ny * s->nz;
    }
}

static void sim_dispatch(VkCommandBuffer cmd, Push p, uint32_t pass, uint32_t fire, const FireState* s) {
    p.pass = pass, p.pad = fire;
    vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, (s->nx + 3) / 4, (s->ny + 3) / 4, (s->nz + 3) / 4);
    F.stats.dispatches++;
}

void fire_record(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push) {
    F.stats.dispatches = 0;
    if (!F.stats.fires || !F.fields.buffer) return;
    VkPipelineStageFlags2 CS = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    VkAccessFlags2 RW = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    if (F.clear) {
        gpu_barrier(cmd, CS, RW, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT);
        vkCmdFillBuffer(cmd, F.fields.buffer, 0, VK_WHOLE_SIZE, 0);
        gpu_barrier(cmd, VK_PIPELINE_STAGE_2_CLEAR_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
        F.clear = false;
    }
    gpu_barrier(cmd, CS, RW, CS, RW); // (last frame's drawing read them)
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 0, 1, &set, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.sim);
    Push p = *push;
    p.instances = F.info[F.slot].addr;
    p.vertices = F.fields.addr;
    for (uint32_t k = 0; k < F.stats.fires; k++) {
        FireState* s = NULL;
        // (the table's order is fire_fill's: find the state by its base)
        const FireGpu* g = &((const FireGpu*)F.info[F.slot].mapped)[k];
        for (uint32_t i = 0; i < F.n && !s; i++)
            if (F.s[i].base == g->dims[3]) s = &F.s[i];
        if (s && s->playback) { // (A9.5a) a volume: its new frame copied in, then the sun's light through it (pass 6)
            if (!s->need_up) continue;
            size_t cells = (size_t)s->nx * s->ny * s->nz;
            VkBufferCopy rg = { 0, ((VkDeviceSize)s->base + 2u * cells) * 16u, cells * 16u };
            vkCmdCopyBuffer(cmd, s->stage[F.slot].buffer, F.fields.buffer, 1, &rg);
            gpu_barrier(cmd, VK_PIPELINE_STAGE_2_COPY_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, CS, RW);
            sim_dispatch(cmd, p, 6, k, s);
            gpu_barrier(cmd, CS, RW, CS, RW);
            s->need_up = false;
            continue;
        }
        if (!s || !s->run) continue;
        for (uint32_t pass = 0; pass < 4; pass++) {
            sim_dispatch(cmd, p, pass, k, s);
            gpu_barrier(cmd, CS, RW, CS, RW);
        }
        for (uint32_t it = 0; it < s->iterations; it++) {
            sim_dispatch(cmd, p, 4u | (it & 1u) << 8, k, s);
            gpu_barrier(cmd, CS, RW, CS, RW);
        }
        sim_dispatch(cmd, p, 5, k, s);
        gpu_barrier(cmd, CS, RW, CS, RW);
        sim_dispatch(cmd, p, 6, k, s);
        gpu_barrier(cmd, CS, RW, CS, RW);
    }
}

void fire_render(VkCommandBuffer cmd, VkDescriptorSet set, const Push* push, uint32_t width, uint32_t height) {
    if (!F.stats.fires || !F.fields.buffer || !F.render) return;
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.layout, 0, 1, &set, 0, NULL);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, F.render);
    Push p = *push;
    p.instances = F.info[F.slot].addr;
    p.vertices = F.fields.addr;
    p.pass = F.stats.fires, p.pad = 0;
    vkCmdPushConstants(cmd, F.layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
    vkCmdDispatch(cmd, (width + 7) / 8, (height + 7) / 8, 1);
}
