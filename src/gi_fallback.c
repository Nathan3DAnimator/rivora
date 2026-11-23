// (Q27.12) The GI's fallback: the AO volume and the ground's bounce (gi_fallback.h)
#include "gi_fallback.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "platform.h"
#include "render.h"
#include "vkctx.h"

#define NCELLS (GIFB_NX * GIFB_NY * GIFB_NZ)
#define MAX_BOXES 65536

static struct {
    GpuBuf buf[2];
    int cur; // the buffer the frames read (-1: none yet)
    // the build: its boxes (world AABBs), its origin, its result; the thread
    float (*box)[6];
    uint32_t nbox;
    float origin[3], built_origin[3], cur_origin[3];
    float* occ;
    uint8_t vis[NCELLS];
    ReThread* th;
    volatile int32_t done; // 1: a build finished, its vis waiting to upload
    bool building;
    double last_start;
    GiFallbackStats st;
} G = { .cur = -1 };

static void rotate_aabb(const float mn[3], const float mx[3], const CTransform* t, float out[6]) {
    float m[3][3];
    rotator_axes(t->rotation, m);
    float c[3], e[3];
    for (int a = 0; a < 3; a++) c[a] = (mn[a] + mx[a]) * 0.5f * t->scale[a], e[a] = (mx[a] - mn[a]) * 0.5f * fabsf(t->scale[a]);
    for (int r = 0; r < 3; r++) {
        float cw = t->location[r], ew = 0.0f;
        for (int a = 0; a < 3; a++) cw += m[r][a] * c[a], ew += fabsf(m[r][a]) * e[a];
        out[r] = cw - ew, out[3 + r] = cw + ew;
    }
}

// The build (its thread): each box's opacity in each cell (its cover of the cell's cross-section), then each cell's sky: 5 rays (up, and four at 45 degrees) stepping 10 cells through it
static int build(void* arg) {
    (void)arg;
    double t0 = re_time_seconds();
    float* occ = G.occ;
    memset(occ, 0, sizeof(float) * NCELLS);
    const float cs = GIFB_CELL, *o = G.origin;
    for (uint32_t b = 0; b < G.nbox; b++) {
        const float* B = G.box[b];
        int lo[3], hi[3], n[3] = { GIFB_NX, GIFB_NY, GIFB_NZ };
        bool out = false;
        for (int a = 0; a < 3 && !out; a++) {
            lo[a] = (int)floorf((B[a] - o[a]) / cs), hi[a] = (int)floorf((B[3 + a] - o[a]) / cs);
            if (hi[a] < 0 || lo[a] >= n[a]) out = true;
            lo[a] = lo[a] < 0 ? 0 : lo[a], hi[a] = hi[a] >= n[a] ? n[a] - 1 : hi[a];
        }
        if (out) continue;
        for (int z = lo[2]; z <= hi[2]; z++)
            for (int y = lo[1]; y <= hi[1]; y++)
                for (int x = lo[0]; x <= hi[0]; x++) {
                    // (its opacity to a ray across the cell: the two largest of its overlaps along the axes - a 20 cm
                    // wall or roof spanning the cell stops the ray as a solid block does; a thin pole barely does)
                    float ov[3], c0[3] = { o[0] + x * cs, o[1] + y * cs, o[2] + z * cs };
                    for (int a = 0; a < 3; a++) ov[a] = fmaxf(fminf(B[3 + a], c0[a] + cs) - fmaxf(B[a], c0[a]), 0.0f) / cs;
                    float mn = fminf(ov[0], fminf(ov[1], ov[2])), f = mn > 0.0f ? ov[0] * ov[1] * ov[2] / mn : 0.0f;
                    float* c = &occ[((size_t)z * GIFB_NY + y) * GIFB_NX + x];
                    *c = fminf(*c + f, 1.0f);
                }
    }
    static const int dirs[5][3] = { { 0, 0, 1 }, { 1, 0, 1 }, { -1, 0, 1 }, { 0, 1, 1 }, { 0, -1, 1 } };
    double sum = 0.0;
    for (int z = 0; z < GIFB_NZ; z++)
        for (int y = 0; y < GIFB_NY; y++)
            for (int x = 0; x < GIFB_NX; x++) {
                float v = 0.0f;
                for (int d = 0; d < 5; d++) {
                    float T = 1.0f;
                    for (int s = 1; s <= 10 && T > 0.02f; s++) {
                        int xx = x + dirs[d][0] * s, yy = y + dirs[d][1] * s, zz = z + dirs[d][2] * s;
                        if (xx < 0 || yy < 0 || xx >= GIFB_NX || yy >= GIFB_NY || zz >= GIFB_NZ) break; // (out: the open sky)
                        T *= 1.0f - 0.9f * occ[((size_t)zz * GIFB_NY + yy) * GIFB_NX + xx];
                    }
                    v += T * (d == 0 ? 0.4f : 0.15f); // (up weighs most: the sky's cosine)
                }
                G.vis[((size_t)z * GIFB_NY + y) * GIFB_NX + x] = (uint8_t)(fminf(fmaxf(v, 0.0f), 1.0f) * 255.0f + 0.5f);
                sum += v;
            }
    G.st.mean_vis = (float)(sum / NCELLS);
    G.st.build_ms = (re_time_seconds() - t0) * 1000.0;
    re_atomic_store(&G.done, 1);
    return 0;
}

static void start(World* w, const float cam[3]) {
    if (!G.box && !(G.box = malloc(sizeof(float) * 6 * MAX_BOXES))) return;
    if (!G.occ && !(G.occ = malloc(sizeof(float) * NCELLS))) return;
    G.nbox = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_STATIC_MESH)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        for (uint32_t i = 0; i < a->count && G.nbox < MAX_BOXES; i++) {
            if (tr[i].flags & TRANSFORM_HIDDEN) continue;
            float mn[3], mx[3];
            render_mesh_bounds(sm[i].mesh, mn, mx);
            rotate_aabb(mn, mx, &tr[i], G.box[G.nbox++]);
        }
    }
    // the grid: 16 m steps, the camera a quarter up from its bottom (more above than below)
    for (int a = 0; a < 2; a++) G.origin[a] = floorf(cam[a] / 1600.0f) * 1600.0f - GIFB_CELL * (a ? GIFB_NY : GIFB_NX) * 0.5f;
    G.origin[2] = floorf(cam[2] / 1600.0f) * 1600.0f - GIFB_CELL * GIFB_NZ * 0.25f;
    G.st.boxes = G.nbox;
    G.building = true, G.done = 0, G.last_start = re_time_seconds();
    G.th = re_thread_start(build, NULL);
    if (!G.th) G.building = false;
}

void gi_fallback_frame(VkCtx* vk, GpuFrame* fr, World* w, const float cam[3]) {
    memset(fr->water[24], 0, sizeof(float) * 4 * 3);
    if (getenv("REALENGINE_NO_GI_FALLBACK")) return; // (the measure without it)
    if (G.building && re_atomic_load(&G.done)) { // a build finished: into the buffer the frames don't read
        re_thread_join(G.th), G.th = NULL, G.building = false;
        int nb = G.cur < 0 ? 0 : 1 - G.cur;
        if (G.buf[nb].buffer || gpu_buf_create(vk, &G.buf[nb], NCELLS, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true)) {
            memcpy(G.buf[nb].mapped, G.vis, NCELLS);
            memcpy(G.cur_origin, G.origin, sizeof G.cur_origin);
            G.cur = nb, G.st.ready = true, G.st.builds++;
        }
    }
    if (!G.building) {
        float dx = cam[0] - (G.built_origin[0]), dy = cam[1] - G.built_origin[1], dz = cam[2] - G.built_origin[2];
        if (G.cur < 0 || dx * dx + dy * dy + dz * dz > 1600.0f * 1600.0f || re_time_seconds() - G.last_start > 2.0) {
            memcpy(G.built_origin, cam, sizeof G.built_origin);
            start(w, cam);
        }
    }
    if (G.cur < 0) return;
    // the volume: water[24] its address (as bits) and origin x, y; [25] origin z, cell, on; [26] the ground's bounce
    // (rgb: albedo 0.2 x the sun's light on the ground / pi, the irradiance it sends up)
    uint32_t lo = (uint32_t)G.buf[G.cur].addr, hi = (uint32_t)(G.buf[G.cur].addr >> 32);
    memcpy(&fr->water[24][0], &lo, 4), memcpy(&fr->water[24][1], &hi, 4);
    fr->water[24][2] = G.cur_origin[0], fr->water[24][3] = G.cur_origin[1];
    fr->water[25][0] = G.cur_origin[2], fr->water[25][1] = GIFB_CELL, fr->water[25][2] = 1.0f;
    // (the sun: the first Directional light's illuminance and color, its height from the frame's sun direction)
    float sun[3] = { 0, 0, 0 };
    for (uint32_t ai = 0; ai < w->archetype_count && sun[0] + sun[1] + sun[2] <= 0.0f; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_LIGHT))) continue;
        const CLight* l = ecs_arch_column(a, COMP_LIGHT);
        for (uint32_t i = 0; i < a->count; i++)
            if (l[i].type == LIGHT_DIRECTIONAL && l[i].intensity > 0.0f) {
                for (int k = 0; k < 3; k++) sun[k] = l[i].intensity * l[i].color[k];
                break;
            }
    }
    float up = fabsf(fr->sun_dir[2]) * (fr->sun_dir[3] > 0.5f ? 1.0f : 0.0f);
    for (int k = 0; k < 3; k++) fr->water[26][k] = 0.2f * sun[k] * up * 0.3183099f;
    fr->gi_flags |= GI_FALLBACK;
}

void gi_fallback_shutdown(VkCtx* vk) {
    if (G.th) re_thread_join(G.th), G.th = NULL;
    for (int k = 0; k < 2; k++)
        if (G.buf[k].buffer) gpu_buf_destroy(vk, &G.buf[k]);
    free(G.box), free(G.occ);
    G.box = NULL, G.occ = NULL, G.cur = -1, G.building = false;
}

const GiFallbackStats* gi_fallback_stats(void) { return &G.st; }
