// (Q27.12) The GI's fallback: the AO volume and the ground's bounce (gi_fallback.h)
#include "gi_fallback.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "lights.h"
#include "material.h"
#include "platform.h"
#include "render.h"
#include "vkctx.h"

#define NCELLS (GIFB_NX * GIFB_NY * GIFB_NZ)
#define FCELLS (GIFL_NX * GIFL_NY * GIFL_NZ)
#define MAX_BOXES 65536
#define BUF_BYTES (NCELLS + FCELLS * 8) // (the AO's bytes, then the bounce's 8 bytes a cell)

static struct {
    GpuBuf buf[2];
    int cur; // the buffer the frames read (-1: none yet)
    // the build: its boxes (world AABBs) and their albedos, its origins, its result; the thread
    float (*box)[6];
    float (*alb)[3];
    uint32_t nbox;
    float origin[3], built_origin[3], cur_origin[3];
    float* occ;
    uint8_t vis[NCELLS];
    // the local lights' bounce: its origin (50 cm cells, snapped to 4 m), the lights (their GPU form), the cells
    float forigin[3], fbuilt[3], fcur[3];
    GpuLight light[GIFL_MAX_LIGHTS];
    uint32_t nlight, light_hash, cur_hash;
    double last_light_check;
    uint8_t* blk;    // a cell's faces toward +x, +y, +z a wall or floor stops (bits 0-2)
    uint8_t* emask;  // its open neighbours (6 bits)
    float* falb;     // the albedo of each of those faces (rgb x 3)
    float* fsum;     // a cell's walls: the light they reflect (rgb), their albedos (rgb)
    float* V;        // a cell's light (rgb: the mean radiance around, E/pi), kept between builds (a warm start)
    uint32_t* fout;  // the upload: rgb in halves + the stopped faces' bits
    bool warm;       // V is from the same origin and walls
    ReThread* th;
    volatile int32_t done; // 1: a build finished, its vis waiting to upload
    bool building;
    double last_start;
    GiFallbackStats st;
} G = { .cur = -1 };

static uint16_t to_half(float f) { // (round to nearest; overflow: the largest half)
    uint32_t b;
    memcpy(&b, &f, sizeof b);
    uint16_t s = (uint16_t)((b >> 16) & 0x8000u);
    float a = fabsf(f);
    if (!(a < 65504.0f)) return (uint16_t)(s | 0x7BFFu);
    if (a < 6.103515625e-5f) return (uint16_t)(s | (uint16_t)(a * 16777216.0f + 0.5f));
    memcpy(&b, &a, sizeof b);
    b += 0x1000u;
    return (uint16_t)(s | (((b >> 23) - 112u) << 10) | ((b >> 13) & 1023u));
}

#define FIDX(x, y, z) (((size_t)(z) * GIFL_NY + (y)) * GIFL_NX + (x))

// A light seen from a cell's center (a, in cells) at b (cells): the stopped faces between them stop it (3D DDA), the
// volume's outside open. (Its lamp's own mesh stops no face: build_fine leaves out the small boxes holding a light; the
// first rule, any face within 75 cm of the light open, let a lamp 30 cm from a 5 cm wall light the far side: spec_leak_low)
static float fine_vis(const float a[3], const float b[3]) {
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, tmax[3], tdel[3];
    int c[3], st[3], n[3] = { GIFL_NX, GIFL_NY, GIFL_NZ };
    for (int k = 0; k < 3; k++) {
        c[k] = (int)floorf(a[k]);
        if (d[k] > 1e-6f) st[k] = 1, tmax[k] = ((float)c[k] + 1.0f - a[k]) / d[k], tdel[k] = 1.0f / d[k];
        else if (d[k] < -1e-6f) st[k] = -1, tmax[k] = (a[k] - (float)c[k]) / -d[k], tdel[k] = -1.0f / d[k];
        else st[k] = 0, tmax[k] = tdel[k] = 1e30f;
    }
    for (int s = 0; s < GIFL_NX + GIFL_NY + GIFL_NZ; s++) {
        int ax = tmax[0] < tmax[1] ? (tmax[0] < tmax[2] ? 0 : 2) : (tmax[1] < tmax[2] ? 1 : 2);
        if (tmax[ax] >= 1.0f) return 1.0f; // (the light's cell reached)
        int lo[3] = { c[0], c[1], c[2] }; // (the face crossed: kept by the lower of its two cells)
        c[ax] += st[ax], tmax[ax] += tdel[ax];
        if (c[ax] < 0 || c[ax] >= n[ax]) return 1.0f;
        if (st[ax] < 0) lo[ax] = c[ax];
        if (G.blk[FIDX(lo[0], lo[1], lo[2])] & (1u << ax)) return 0.0f;
    }
    return 1.0f;
}

// The irradiance a light gives a wall face at p (cm) facing nrm (as mesh.frag's falloff and cone), unshadowed
static float face_light(const GpuLight* L, const float p[3], const float nrm[3], float lv[3]) {
    float l[3] = { L->position[0] - p[0], L->position[1] - p[1], L->position[2] - p[2] };
    float d = sqrtf(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (d < 1e-3f) return 0.0f;
    for (int k = 0; k < 3; k++) l[k] /= d;
    float cs = l[0] * nrm[0] + l[1] * nrm[1] + l[2] * nrm[2];
    if (cs <= 0.0f) return 0.0f;
    float w = 1.0f - (d * d * L->inv_range2) * (d * d * L->inv_range2);
    if (w <= 0.0f) return 0.0f;
    w = w > 1.0f ? 1.0f : w;
    float dm = fmaxf(d * 0.01f, fmaxf(L->source_radius * 0.01f, 0.01f)), at = w * w / (dm * dm) * cs;
    uint32_t kind = L->type & 3u;
    float ld = -(l[0] * L->direction[0] + l[1] * L->direction[1] + l[2] * L->direction[2]);
    if (kind == 2u) { // spot: its cone
        float c = (ld - L->cos_outer) / fmaxf(L->cos_inner - L->cos_outer, 1e-4f);
        c = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
        at *= c * c;
    } else if (kind == 3u) at *= fmaxf(ld, 0.0f); // rect: its face
    memcpy(lv, l, sizeof(float) * 3);
    return at;
}

// The bounce's volume (on the build's thread, after the AO): the boxes into stopped faces between the cells (a wall
// stops a plane of faces, not cells: a 20 cm wall made 1 m of solid cells, and the floor at its foot read none: black
// stripes), the lights on those faces, then the cells solved (red-black over-relaxed sweeps) for the light around them
static void build_fine(void) {
    double t0 = re_time_seconds();
    const float cs = GIFL_CELL, *o = G.forigin;
    const int n[3] = { GIFL_NX, GIFL_NY, GIFL_NZ };
    uint8_t* blk = G.blk;
    float* best = G.fsum; // (each face's best cover so far: the albedo of the box covering it most; 3 a cell)
    memset(blk, 0, FCELLS);
    memset(best, 0, sizeof(float) * 3 * FCELLS);
    // the boxes into stopped faces: along each axis, the cell planes across the box's thickness (the nearest one to its
    // middle when it is thinner than a cell: a 20 cm wall stops one plane), each face there it covers at least half of
    for (uint32_t b = 0; b < G.nbox; b++) {
        const float* B = G.box[b];
        int lo[3], hi[3];
        bool out = false;
        for (int a = 0; a < 3 && !out; a++) {
            lo[a] = (int)floorf((B[a] - o[a]) / cs), hi[a] = (int)floorf((B[3 + a] - o[a]) / cs);
            if (hi[a] < 0 || lo[a] >= n[a]) out = true;
            lo[a] = lo[a] < 0 ? 0 : lo[a], hi[a] = hi[a] >= n[a] ? n[a] - 1 : hi[a];
        }
        if (out) continue;
        // (a lamp's housing: a box under 1.5 m holding a light stops nothing, or the lamp would light only its inside)
        bool housing = false;
        if (B[3] - B[0] < 150.0f && B[4] - B[1] < 150.0f && B[5] - B[2] < 150.0f)
            for (uint32_t k = 0; k < G.nlight && !housing; k++) {
                const float* lp = G.light[k].position;
                housing = lp[0] >= B[0] - 5.0f && lp[0] <= B[3] + 5.0f && lp[1] >= B[1] - 5.0f && lp[1] <= B[4] + 5.0f && lp[2] >= B[2] - 5.0f &&
                          lp[2] <= B[5] + 5.0f;
            }
        if (housing) continue;
        for (int a = 0; a < 3; a++) {
            int a1 = (a + 1) % 3, a2 = (a + 2) % 3;
            int p0 = (int)ceilf((B[a] - o[a]) / cs), p1 = (int)floorf((B[3 + a] - o[a]) / cs);
            if (p0 > p1) p0 = p1 = (int)lroundf(((B[a] + B[3 + a]) * 0.5f - o[a]) / cs);
            if (p0 < 1) p0 = 1;
            if (p1 > n[a] - 1) p1 = n[a] - 1;
            for (int u = lo[a1]; u <= hi[a1]; u++) {
                float c1 = o[a1] + u * cs, ov1 = fmaxf(fminf(B[3 + a1], c1 + cs) - fmaxf(B[a1], c1), 0.0f) / cs;
                if (ov1 <= 0.0f) continue;
                for (int v = lo[a2]; v <= hi[a2]; v++) {
                    float c2 = o[a2] + v * cs, ov2 = fmaxf(fminf(B[3 + a2], c2 + cs) - fmaxf(B[a2], c2), 0.0f) / cs;
                    float cover = ov1 * ov2;
                    if (cover < 0.5f) continue;
                    for (int p = p0; p <= p1; p++) {
                        int c[3];
                        c[a] = p - 1, c[a1] = u, c[a2] = v; // (the face between cells p - 1 and p: kept by p - 1)
                        size_t i = FIDX(c[0], c[1], c[2]);
                        if (!(blk[i] & (1u << a)) || cover > best[i * 3 + a])
                            memcpy(&G.falb[(i * 3 + a) * 3], G.alb[b], sizeof(float) * 3), best[i * 3 + a] = cover;
                        blk[i] |= (uint8_t)(1u << a);
                    }
                }
            }
        }
    }
    // the walls around each cell: their light (each light's at the face, the cell's view of the light shadowed once)
    // and their albedos; the open neighbours' mask
    static const int D[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    const ptrdiff_t off[6] = { 1, -1, GIFL_NX, -GIFL_NX, GIFL_NX * GIFL_NY, -(ptrdiff_t)(GIFL_NX * GIFL_NY) };
    float lc[GIFL_MAX_LIGHTS][3], ind[GIFL_MAX_LIGHTS];
    for (uint32_t k = 0; k < G.nlight; k++) {
        for (int a = 0; a < 3; a++) lc[k][a] = (G.light[k].position[a] - o[a]) / cs;
        ind[k] = (float)((G.light[k].type >> 24) & 255u) / 64.0f;
    }
    uint32_t faces = 0;
    float* S = G.fsum;
    for (int z = 0; z < n[2]; z++)
        for (int y = 0; y < n[1]; y++)
            for (int x = 0; x < n[0]; x++) {
                size_t i = FIDX(x, y, z);
                float* s = &S[i * 6];
                memset(s, 0, sizeof(float) * 6);
                G.emask[i] = 0;
                float ctr[3] = { x + 0.5f, y + 0.5f, z + 0.5f }, vis[GIFL_MAX_LIGHTS];
                for (uint32_t k = 0; k < G.nlight; k++) vis[k] = -1.0f; // (not traced yet)
                for (int f = 0; f < 6; f++) {
                    int q[3] = { x + D[f][0], y + D[f][1], z + D[f][2] }, a = f >> 1;
                    if (q[0] < 0 || q[1] < 0 || q[2] < 0 || q[0] >= n[0] || q[1] >= n[1] || q[2] >= n[2]) continue;
                    size_t lo = (f & 1) ? (size_t)((ptrdiff_t)i + off[f]) : i; // (the face's keeper: the lower cell)
                    if (!(blk[lo] & (1u << a))) {
                        G.emask[i] |= (uint8_t)(1u << f);
                        continue;
                    }
                    const float* al = &G.falb[(lo * 3 + a) * 3];
                    for (int c = 0; c < 3; c++) s[3 + c] += al[c];
                    // the face (cm), facing this cell
                    float p[3], nrm[3] = { (float)-D[f][0], (float)-D[f][1], (float)-D[f][2] }, E[3] = { 0, 0, 0 };
                    for (int c = 0; c < 3; c++) p[c] = o[c] + (ctr[c] + 0.5f * (float)D[f][c]) * cs;
                    for (uint32_t k = 0; k < G.nlight; k++) {
                        float lv[3], at = face_light(&G.light[k], p, nrm, lv);
                        if (at <= 0.0f) continue;
                        if (vis[k] < 0.0f) vis[k] = fine_vis(ctr, lc[k]);
                        if (vis[k] <= 0.0f) continue;
                        for (int c = 0; c < 3; c++) E[c] += G.light[k].radiance[c] * at * vis[k] * ind[k];
                    }
                    if (E[0] + E[1] + E[2] > 0.0f) faces++;
                    for (int c = 0; c < 3; c++) s[c] += al[c] * E[c] * 0.3183099f; // (the radiance it reflects: albedo E / pi)
                }
            }
    // the solve: each cell the mean of its six neighbours - an open one's light, a wall's own light plus its albedo
    // times this cell's (the bounces), the volume's outside dark (the light leaves): V (6 - sum of albedos) = its open
    // neighbours + its walls' light. Warm: from the last build's V (the same cells), fewer sweeps
    float* V = G.V;
    if (!G.warm) memset(V, 0, sizeof(float) * 3 * FCELLS);
    int iters = G.warm ? 40 : 160, it = 0;
    const float omega = 1.8f;
    for (; it < iters; it++) {
        float dmax = 0.0f, vmax = 0.0f;
        for (int par = 0; par < 2; par++)
            for (int z = 0; z < n[2]; z++)
                for (int y = 0; y < n[1]; y++)
                    for (int x = (y + z + par) & 1; x < n[0]; x += 2) {
                        size_t i = FIDX(x, y, z);
                        const float* s = &S[i * 6];
                        float sum[3] = { s[0], s[1], s[2] };
                        uint8_t m = G.emask[i];
                        for (int f = 0; f < 6; f++)
                            if (m & (1u << f)) {
                                const float* v = &V[(size_t)((ptrdiff_t)i + off[f]) * 3];
                                sum[0] += v[0], sum[1] += v[1], sum[2] += v[2];
                            }
                        float* v = &V[i * 3];
                        for (int a = 0; a < 3; a++) {
                            float nv = sum[a] / fmaxf(6.0f - s[3 + a], 0.3f), dv = omega * (nv - v[a]);
                            v[a] = fmaxf(v[a] + dv, 0.0f);
                            dmax = fmaxf(dmax, fabsf(dv)), vmax = fmaxf(vmax, v[a]);
                        }
                    }
        if (dmax <= vmax * 1e-4f) break;
    }
    double sum = 0.0;
    float mx = 0.0f;
    for (size_t i = 0; i < FCELLS; i++) {
        const float* v = &V[i * 3];
        float y = 0.2126f * v[0] + 0.7152f * v[1] + 0.0722f * v[2];
        sum += y, mx = fmaxf(mx, y);
        G.fout[i * 2] = (uint32_t)to_half(v[0]) | (uint32_t)to_half(v[1]) << 16;
        G.fout[i * 2 + 1] = (uint32_t)to_half(v[2]) | (uint32_t)blk[i] << 16;
    }
    G.st.fine_lights = G.nlight, G.st.fine_faces = faces, G.st.fine_iters = (uint32_t)it;
    G.st.fine_mean = (float)(sum / FCELLS), G.st.fine_max = mx;
    G.st.fine_ms = (re_time_seconds() - t0) * 1000.0;
}

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
    build_fine();
    G.st.build_ms = (re_time_seconds() - t0) * 1000.0;
    re_atomic_store(&G.done, 1);
    return 0;
}

// The bounce volume's origin for a camera: 4 m steps across, 2 m up (the camera 3-5 m above its bottom); always inside
// the AO's volume, at 96..120 cells across and 10..38 up from its origin (packed in a byte each: gi_fallback_frame)
static void fine_origin(const float cam[3], float o[3]) {
    for (int a = 0; a < 2; a++) o[a] = floorf(cam[a] / 400.0f) * 400.0f - GIFL_CELL * (a ? GIFL_NY : GIFL_NX) * 0.5f;
    o[2] = floorf(cam[2] / 200.0f) * 200.0f - 300.0f;
}

// The local lights that can light the bounce volume, as the GPU has them (the brightest for their distance first)
static uint32_t gather_lights(World* w, const float fo[3], GpuLight* out, uint32_t cap) {
    float score[GIFL_MAX_LIGHTS];
    uint32_t nl = 0;
    const float ctr[3] = { fo[0] + GIFL_CELL * GIFL_NX * 0.5f, fo[1] + GIFL_CELL * GIFL_NY * 0.5f, fo[2] + GIFL_CELL * GIFL_NZ * 0.5f };
    const float half = 0.5f * sqrtf((float)(GIFL_NX * GIFL_NX + GIFL_NY * GIFL_NY + GIFL_NZ * GIFL_NZ)) * GIFL_CELL;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_LIGHT)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const Entity* ents = ecs_arch_entities(a);
        const CLight* cl = ecs_arch_column(a, COMP_LIGHT);
        const CTransform* ct = ecs_arch_column(a, COMP_TRANSFORM);
        for (uint32_t i = 0; i < a->count; i++) {
            // (out of reach first, from the columns: 5000 lights through lights_gpu 4 times a second made a 1.7 ms frame)
            if (cl[i].type == LIGHT_DIRECTIONAL || cl[i].type == LIGHT_SKY || cl[i].intensity <= 0.0f) continue; // (the sun is the AO's)
            float d2 = 0.0f;
            for (int k = 0; k < 3; k++) d2 += (ct[i].location[k] - ctr[k]) * (ct[i].location[k] - ctr[k]);
            float reach = fmaxf(cl[i].attenuation_radius, 1.0f) + half;
            if (d2 > reach * reach) continue;
            GpuLight g;
            if (!lights_gpu(w, ents[i], true, &g) || (g.type & 3u) == 0u) continue;
            float s = (0.2126f * g.radiance[0] + 0.7152f * g.radiance[1] + 0.0722f * g.radiance[2]) / fmaxf(d2 * 1e-4f, 1.0f);
            if (s <= 0.0f) continue;
            uint32_t at = nl;
            if (nl == cap) { // full: in place of the dimmest
                at = 0;
                for (uint32_t k = 1; k < nl; k++)
                    if (score[k] < score[at]) at = k;
                if (score[at] >= s) continue;
            } else nl++;
            out[at] = g, score[at] = s;
        }
    }
    return nl;
}

// A hash of the lights (their place, color and reach, in coarse steps): a change starts a build
static uint32_t lights_hash(const GpuLight* l, uint32_t n) {
    uint32_t h = 2166136261u ^ n;
    for (uint32_t i = 0; i < n; i++) {
        float v[14] = { l[i].position[0], l[i].position[1], l[i].position[2], l[i].direction[0] * 64.0f, l[i].direction[1] * 64.0f,
                        l[i].direction[2] * 64.0f, l[i].range, l[i].cos_outer * 64.0f, l[i].cos_inner * 64.0f,
                        (float)(l[i].type >> 24) };
        for (int k = 0; k < 3; k++) v[10 + k] = l[i].radiance[k] > 0.0f ? log2f(l[i].radiance[k]) * 8.0f : -1000.0f;
        v[13] = 0.0f;
        for (int k = 0; k < 14; k++) h = (h ^ (uint32_t)(int32_t)floorf(v[k] * 0.1f)) * 16777619u;
    }
    return h;
}

static void start(World* w, const float cam[3]) {
    if (!G.box && !(G.box = malloc(sizeof(float) * 6 * MAX_BOXES))) return;
    if (!G.alb && !(G.alb = malloc(sizeof(float) * 3 * MAX_BOXES))) return;
    if (!G.occ && !(G.occ = malloc(sizeof(float) * NCELLS))) return;
    if (!G.blk) {
        G.blk = malloc(FCELLS), G.emask = malloc(FCELLS), G.falb = malloc(sizeof(float) * 9 * FCELLS);
        G.fsum = malloc(sizeof(float) * 6 * FCELLS), G.V = malloc(sizeof(float) * 3 * FCELLS), G.fout = malloc(8u * FCELLS);
        if (!G.blk || !G.emask || !G.falb || !G.fsum || !G.V || !G.fout) {
            free(G.blk), free(G.emask), free(G.falb), free(G.fsum), free(G.V), free(G.fout);
            G.blk = G.emask = NULL, G.falb = G.fsum = G.V = NULL, G.fout = NULL;
            return;
        }
        G.warm = false;
    }
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
            // (its albedo for the bounce: its material's baked mean, else its color)
            const Material* m = sm[i].material ? mat_find_hash(sm[i].material) : NULL;
            const float* c = m ? m->avg_base : sm[i].base_color;
            for (int k = 0; k < 3; k++) G.alb[G.nbox][k] = fminf(fmaxf(c[k], 0.0f), 0.95f) * (1.0f - fminf(fmaxf(sm[i].metallic, 0.0f), 1.0f));
            rotate_aabb(mn, mx, &tr[i], G.box[G.nbox++]);
        }
    }
    // the grid: 16 m steps, the camera a quarter up from its bottom (more above than below)
    for (int a = 0; a < 2; a++) G.origin[a] = floorf(cam[a] / 1600.0f) * 1600.0f - GIFB_CELL * (a ? GIFB_NY : GIFB_NX) * 0.5f;
    G.origin[2] = floorf(cam[2] / 1600.0f) * 1600.0f - GIFB_CELL * GIFB_NZ * 0.25f;
    float fo[3];
    fine_origin(cam, fo);
    G.warm = G.st.builds > 0 && !memcmp(fo, G.forigin, sizeof fo); // (the same cells: the last light a start)
    memcpy(G.forigin, fo, sizeof fo);
    G.nlight = gather_lights(w, fo, G.light, GIFL_MAX_LIGHTS);
    G.light_hash = lights_hash(G.light, G.nlight);
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
        if (G.buf[nb].buffer || gpu_buf_create(vk, &G.buf[nb], BUF_BYTES, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT, true)) {
            memcpy(G.buf[nb].mapped, G.vis, NCELLS);
            memcpy((uint8_t*)G.buf[nb].mapped + NCELLS, G.fout, 8u * FCELLS);
            memcpy(G.cur_origin, G.origin, sizeof G.cur_origin);
            memcpy(G.fcur, G.forigin, sizeof G.fcur);
            G.cur_hash = G.light_hash;
            G.cur = nb, G.st.ready = true, G.st.builds++;
        }
    }
    if (!G.building) {
        float dx = cam[0] - (G.built_origin[0]), dy = cam[1] - G.built_origin[1], dz = cam[2] - G.built_origin[2], fo[3];
        double now = re_time_seconds();
        fine_origin(cam, fo);
        bool lit = false; // (a light changed: its bounce again, at most 4 times a second)
        if (G.cur >= 0 && now - G.last_light_check > 0.25) {
            GpuLight l[GIFL_MAX_LIGHTS];
            G.last_light_check = now;
            uint32_t nl = gather_lights(w, fo, l, GIFL_MAX_LIGHTS);
            lit = lights_hash(l, nl) != G.cur_hash;
        }
        if (G.cur < 0 || dx * dx + dy * dy + dz * dz > 1600.0f * 1600.0f || now - G.last_start > 2.0 || lit ||
            memcmp(fo, G.fcur, sizeof fo) != 0) {
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
    // (the bounce volume: after the AO's bytes; its origin in its cells from the AO's, a byte each in [25].w, the top
    // byte 0x40 to keep the float a normal one; [26].w on)
    uint32_t ox = (uint32_t)lroundf((G.fcur[0] - G.cur_origin[0]) / GIFL_CELL), oy = (uint32_t)lroundf((G.fcur[1] - G.cur_origin[1]) / GIFL_CELL);
    uint32_t oz = (uint32_t)lroundf((G.fcur[2] - G.cur_origin[2]) / GIFL_CELL);
    if (ox < 256u && oy < 256u && oz < 256u && !getenv("REALENGINE_NO_GI_BOUNCE")) { // (the env: the measure without it)
        uint32_t pk = 0x40000000u | ox | oy << 8 | oz << 16;
        memcpy(&fr->water[25][3], &pk, 4);
        // [26].w: how far out along the normal it reads (cm) x 16 + how much it leans toward where the light grows
        static float ofs = -1.0f, lean;
        if (ofs < 0.0f) {
            const char* e = getenv("REALENGINE_GI_BOUNCE_OFS"); // (the tuning: tools/gi_bounce_tune.ps1)
            const char* k = getenv("REALENGINE_GI_BOUNCE_LEAN");
            ofs = e ? fminf(fmaxf((float)atof(e), 0.0f), 150.0f) : 25.0f;
            // (25 cm and 6: the house's Low view nearest its High one, 26.8 mean difference against 31.3 at 25/1 and
            // 32.0 at 50/1; 10 and up show the cells' steps)
            lean = k ? fminf(fmaxf((float)atof(k), 0.0f), 15.0f) : 6.0f;
        }
        fr->water[26][3] = floorf(ofs) * 16.0f + lean + (ofs < 1.0f && lean <= 0.0f ? 0.001f : 0.0f);
    }
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
    free(G.box), free(G.alb), free(G.occ);
    free(G.blk), free(G.emask), free(G.falb), free(G.fsum), free(G.V), free(G.fout);
    G.box = NULL, G.alb = NULL, G.occ = NULL, G.cur = -1, G.building = false;
    G.blk = G.emask = NULL, G.falb = G.fsum = G.V = NULL, G.fout = NULL, G.warm = false;
    G.st.builds = 0;
}

const GiFallbackStats* gi_fallback_stats(void) { return &G.st; }
