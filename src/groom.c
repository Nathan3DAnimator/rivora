#include "groom.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "bp_internal.h"
#include "components.h"
#include "material.h"
#include "platform_files.h"
#include "ikrig.h"
#include "platform.h"
#include "render.h"
#include "vkctx.h"

#define GROOM_MAX 256 // (Q27.3: a crowd's heads; before 64)
// (Q27.3) The hair's budget for many characters (GTA 6 grows strands on every NPC): the strand segments drawn a frame,
// shared by every groom (each one's wish by its size on the screen, all scaled down together past it, the strands
// left wider: the same coverage), and the dynamic ones simulated: the nearest HAIR_SIM_MAX, the rest drawn as grown
#define HAIR_BUDGET_SEGMENTS 1500000u
#define HAIR_SIM_MAX 16u
#define GROOM_GRID 32 // (hair.glsl HAIR_GRID)

// hair.glsl GroomInfo (scalar)
typedef struct GpuGroom {
    float rows[3][4], inv[3][4];
    uint32_t first, strands, pts, drawn;
    float root_w, tip_w, cover, ao;
    float color[4];
    float bmin[4], bmax[4];
    float prev[3][4]; // (2026-11-15) last frame's rows: the velocity pass (hair.vert HAIR_VELOCITY), TSR and DLSS see it move
    uint32_t dyn, prev_first, pad0, pad1; // (2026-11-16) simulated: world points, last step's
} GpuGroom;
_Static_assert(sizeof(GpuGroom) == 240, "hair.glsl GroomInfo");

// (2026-11-16) hair_sim.comp HairSim (scalar)
typedef struct HairSimGpu {
    float rows[3][4], inv[3][4];
    uint32_t rest, cur, prev, strands;
    uint32_t pts, ncap, nbox, reset;
    float dt, stiff, damp, grav;
    float wind[4];
    float ell_c[4], ell_r[4];
    float caps[32][8];
    float boxes[16][8];
} HairSimGpu;
static const uint32_t s_hair_sim[] =
#include "hair_sim.comp.inc"
    ;

// A groom grown: its strands (vec4: actor space cm, w the place along it) and its density grid (1/cm), on the CPU until
// the next repack copies them all into the staging buffer
typedef struct Grown {
    Entity e;
    uint32_t key;
    float* pts;
    float* dens;
    uint32_t strands, pts_per, first, dens_first; // (first: in vec4s of the device buffer; dens_first: in floats)
    float bmin[3], bmax[3], center[3], radius;
    float prev_rows[3][4]; // (2026-11-15) its rows last frame (the hair's motion); has_prev: set
    bool has_prev;
    bool used;
    // (2026-11-16) dynamic: its simulated points (vec4 indexes in the device buffer: current, last step's), the head's
    // ellipsoid it grew on (actor space), to start over (first frame, a teleport, a repack)
    bool dyn, sim_reset;
    uint32_t sim_cur, sim_prev;
    float ell_c[3], ell_r[3];
} Grown;

static struct {
    struct VkCtx* vk;
    GpuBuf data, staging, info[RE_FRAMES_IN_FLIGHT];
    VkDeviceSize cap, used;
    bool upload;
    Grown g[GROOM_MAX];
    uint32_t ng;
    uint32_t ndraw, slot;
    bool moving; // (2026-11-15) a groom moved this frame: the velocity pass draws the hair
    uint32_t verts[GROOM_MAX];
    GroomStats stats;
    // (2026-11-16) the simulation: its pipeline, each frame's parameters (one HairSimGpu a dynamic groom), what to run
    VkPipeline sim_pipe;
    GpuBuf simp[RE_FRAMES_IN_FLIGHT];
    uint32_t nsim, sim_grown[GROOM_MAX];
    double last_t;
    float dt;
} G;

const char* const g_groom_preset_names[GROOM_PRESET_COUNT] = { "Custom", "Short", "Long", "Wavy", "Curly" };

void groom_apply_preset(CGroom* g, int preset) {
    g->preset = (uint32_t)preset;
    switch (preset) {
    case GROOM_SHORT: g->length = 5.0f, g->length_var = 0.25f, g->curl = 0.1f, g->curl_freq = 1.0f, g->gravity = 0.25f, g->clump = 0.3f, g->segments = 8; break;
    case GROOM_LONG: g->length = 22.0f, g->length_var = 0.3f, g->curl = 0.4f, g->curl_freq = 2.0f, g->gravity = 0.85f, g->clump = 0.45f, g->segments = 14; break;
    case GROOM_WAVY: g->length = 20.0f, g->length_var = 0.3f, g->curl = 1.2f, g->curl_freq = 3.0f, g->gravity = 0.7f, g->clump = 0.6f, g->segments = 20; break;
    case GROOM_CURLY: g->length = 11.0f, g->length_var = 0.35f, g->curl = 1.0f, g->curl_freq = 7.0f, g->gravity = 0.2f, g->clump = 0.7f, g->segments = 28; break;
    default: break;
    }
}

const GroomStats* groom_stats(void) { return &G.stats; }

bool groom_init(struct VkCtx* vk, VkPipelineLayout layout) {
    memset(&G, 0, sizeof G);
    G.vk = vk;
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++)
        if (!gpu_buf_create(vk, &G.info[i], sizeof(GpuGroom) * GROOM_MAX, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true) ||
            !gpu_buf_create(vk, &G.simp[i], sizeof(HairSimGpu) * GROOM_MAX, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, true)) return false;
    G.sim_pipe = gpu_compute_pipeline(vk, layout, s_hair_sim, sizeof s_hair_sim);
    return G.sim_pipe != VK_NULL_HANDLE;
}

static void grown_free(Grown* g) {
    free(g->pts), free(g->dens);
    memset(g, 0, sizeof *g);
}

void groom_shutdown(struct VkCtx* vk) {
    for (uint32_t i = 0; i < G.ng; i++) grown_free(&G.g[i]);
    gpu_buf_destroy(vk, &G.data), gpu_buf_destroy(vk, &G.staging);
    for (int i = 0; i < RE_FRAMES_IN_FLIGHT; i++) gpu_buf_destroy(vk, &G.info[i]), gpu_buf_destroy(vk, &G.simp[i]);
    if (G.sim_pipe) vkDestroyPipeline(vk->device, G.sim_pipe, NULL);
    memset(&G, 0, sizeof G);
}

static uint32_t xs(uint32_t* s) {
    uint32_t x = *s;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    return *s = x ? x : 0x9E3779B9u;
}
static float rnd(uint32_t* s) { return (float)(xs(s) >> 8) * (1.0f / 16777216.0f); }
static uint32_t hashf(uint32_t h, float v) {
    uint32_t b;
    memcpy(&b, &v, 4);
    return (h ^ b) * 16777619u;
}
static void norm3(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-12f) v[0] /= l, v[1] /= l, v[2] /= l;
}

// a root on the ellipsoid (unit direction d): over the scalp's height, behind the hairline
static bool root_dir(uint32_t* s, const CGroom* g, float d[3]) {
    for (int k = 0; k < 64; k++) {
        float z = 2.0f * rnd(s) - 1.0f, ph = 6.2831853f * rnd(s), r = sqrtf(fmaxf(0.0f, 1.0f - z * z));
        d[0] = r * cosf(ph), d[1] = r * sinf(ph), d[2] = z;
        if (z < g->scalp) continue;
        if (d[0] > g->hairline && z < 0.6f) continue; // (the face: the actor's front, below the crown)
        return true;
    }
    return false;
}
// out of the head: a point inside the ellipsoid (scaled 1.02) onto its surface
static bool keep_out(const float c[3], const float r[3], float p[3], float nn[3]) {
    float q[3] = { (p[0] - c[0]) / r[0], (p[1] - c[1]) / r[1], (p[2] - c[2]) / r[2] };
    float ql = sqrtf(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
    if (ql >= 1.02f || ql < 1e-6f) return false;
    for (int k = 0; k < 3; k++) p[k] = c[k] + r[k] * q[k] * (1.02f / ql), nn[k] = q[k] / r[k];
    norm3(nn);
    return true;
}
// One strand: from the root along the surface's normal, each segment turned toward the ground by gravity and kept
// out of the head (sliding over it), then curled around its own path (more toward the tip)
static void chain(const CGroom* g, const float c[3], const float r[3], const float d[3], float len, uint32_t S, uint32_t* s, float* out) {
    float p[3], dir[3], nn[3];
    float n[3];
    for (int k = 0; k < 3; k++) p[k] = c[k] + r[k] * d[k] * 0.99f, n[k] = d[k] / r[k];
    norm3(n);
    // combed: along the surface, down its slope and back (away from the face, +X), lifted a little off it
    float cmb[3] = { d[0] - 0.9f, d[1], -1.0f };
    float cn = cmb[0] * n[0] + cmb[1] * n[1] + cmb[2] * n[2];
    for (int k = 0; k < 3; k++) cmb[k] -= n[k] * cn;
    norm3(cmb);
    for (int k = 0; k < 3; k++) dir[k] = cmb[k] + n[k] * 0.3f + (rnd(s) - 0.5f) * 0.25f;
    norm3(dir);
    float seg = len / (float)S, gk = g->gravity * 0.6f / (float)S;
    out[0] = p[0], out[1] = p[1], out[2] = p[2], out[3] = 0.0f;
    for (uint32_t i = 1; i <= S; i++) {
        dir[2] -= gk * 2.5f;
        norm3(dir);
        for (int k = 0; k < 3; k++) p[k] += dir[k] * seg;
        if (keep_out(c, r, p, nn)) {
            float dn = dir[0] * nn[0] + dir[1] * nn[1] + dir[2] * nn[2];
            if (dn < 0.0f) dir[0] -= nn[0] * dn, dir[1] -= nn[1] * dn, dir[2] -= nn[2] * dn, norm3(dir);
        }
        float* o = out + 4 * i;
        o[0] = p[0], o[1] = p[1], o[2] = p[2], o[3] = (float)i / (float)S;
    }
    if (g->curl > 0.0f) { // the curl: a helix around the path, its phase the strand's own
        float ph = 6.2831853f * rnd(s);
        for (uint32_t i = 1; i <= S; i++) {
            float* o = out + 4 * i;
            float* q = out + 4 * (i - 1);
            float t[3] = { o[0] - q[0], o[1] - q[1], o[2] - q[2] };
            norm3(t);
            float up[3] = { 0.0f, 0.0f, 1.0f };
            if (fabsf(t[2]) > 0.9f) up[0] = 1.0f, up[2] = 0.0f;
            float u[3] = { t[1] * up[2] - t[2] * up[1], t[2] * up[0] - t[0] * up[2], t[0] * up[1] - t[1] * up[0] };
            norm3(u);
            float v[3] = { t[1] * u[2] - t[2] * u[1], t[2] * u[0] - t[0] * u[2], t[0] * u[1] - t[1] * u[0] };
            float tt = o[3], a = 6.2831853f * g->curl_freq * tt + ph, amp = g->curl * fminf(tt * 3.0f, 1.0f);
            for (int k = 0; k < 3; k++) o[k] += (u[k] * cosf(a) + v[k] * sinf(a)) * amp;
        }
    }
}

// --- Guides (.rgroom, 2026-11-16) ---
// "RGRM", version 1, the guides' count, the suggested look (strands, segments, widths mm, color), then each guide: its
// points' count and its points (cm, the actor's space)
#define RGRM_MAGIC 0x4D524752u
typedef struct Guides { uint32_t hash, n, total; uint32_t* start; float* pts; GroomGuidesInfo info; } Guides;
static Guides g_guides[16];
static uint32_t g_nguides;

uint32_t groom_guides_hash(const char* vpath) {
    uint32_t h = 2166136261u;
    for (const char* c = vpath; *c; c++) h = (h ^ (uint8_t)((*c >= 'A' && *c <= 'Z') ? *c + 32 : *c)) * 16777619u;
    return h ? h : 1u;
}
bool groom_guides_write(const char* disk_path, const float* pts, const uint32_t* counts, uint32_t nguides, const GroomGuidesInfo* info) {
    FILE* f = re_fopen(disk_path, "wb");
    if (!f) return false;
    uint32_t hdr[3] = { RGRM_MAGIC, 1u, nguides };
    bool ok = fwrite(hdr, sizeof hdr, 1, f) == 1 && fwrite(info, sizeof *info, 1, f) == 1;
    size_t at = 0;
    for (uint32_t i = 0; i < nguides && ok; i++) {
        ok = fwrite(&counts[i], 4, 1, f) == 1 && fwrite(pts + at * 3, 12, counts[i], f) == counts[i];
        at += counts[i];
    }
    ok = (fclose(f) == 0) && ok;
    for (uint32_t i = 0; i < g_nguides; i++) g_guides[i].hash = 0; // (written again: read again)
    return ok;
}
// the Content's .rgroom files, by their hash (a walk the first time one is missing)
typedef struct GWalk { uint32_t want; char dir[900], vdir[900], found[1000]; int depth; } GWalk;
static void guides_walk(void* user, const ReDirEntry* e) {
    GWalk* w = user;
    if (w->found[0] || e->name[0] == '.') return;
    if (e->is_dir) {
        if (w->depth >= 10) return;
        GWalk sub = *w;
        sub.depth++;
        snprintf(sub.dir, sizeof sub.dir, "%s/%s", w->dir, e->name), snprintf(sub.vdir, sizeof sub.vdir, "%s/%s", w->vdir, e->name);
        re_dir_walk(sub.dir, guides_walk, &sub);
        if (sub.found[0]) snprintf(w->found, sizeof w->found, "%s", sub.found);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot || _stricmp(dot, ".rgroom")) return;
    char vp[1000];
    snprintf(vp, sizeof vp, "%s/%s", w->vdir, e->name);
    if (groom_guides_hash(vp) == w->want) snprintf(w->found, sizeof w->found, "%s/%s", w->dir, e->name);
}
typedef struct GList { GroomGuidesEntry* out; uint32_t max, n; char dir[900], vdir[600]; int depth; } GList;
static void list_walk(void* user, const ReDirEntry* e) {
    GList* l = user;
    if (e->name[0] == '.' || l->n >= l->max) return;
    if (e->is_dir) {
        if (l->depth >= 10) return;
        GList sub = *l;
        sub.depth++;
        snprintf(sub.dir, sizeof sub.dir, "%s/%s", l->dir, e->name), snprintf(sub.vdir, sizeof sub.vdir, "%s/%s", l->vdir, e->name);
        re_dir_walk(sub.dir, list_walk, &sub);
        l->n = sub.n;
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot || _stricmp(dot, ".rgroom")) return;
    GroomGuidesEntry* o = &l->out[l->n++];
    snprintf(o->name, sizeof o->name, "%.*s", (int)(dot - e->name), e->name);
    snprintf(o->vpath, sizeof o->vpath, "%s/%s", l->vdir, e->name);
}
uint32_t groom_guides_list(GroomGuidesEntry* out, uint32_t max) {
    GList l;
    memset(&l, 0, sizeof l);
    l.out = out, l.max = max;
    snprintf(l.dir, sizeof l.dir, "%s", mat_root()), snprintf(l.vdir, sizeof l.vdir, "/Content");
    re_dir_walk(l.dir, list_walk, &l);
    return l.n;
}
static const Guides* guides_get(uint32_t hash) {
    if (!hash) return NULL;
    for (uint32_t i = 0; i < g_nguides; i++)
        if (g_guides[i].hash == hash) return &g_guides[i];
    GWalk wk;
    memset(&wk, 0, sizeof wk);
    wk.want = hash;
    snprintf(wk.dir, sizeof wk.dir, "%s", mat_root()), snprintf(wk.vdir, sizeof wk.vdir, "/Content");
    re_dir_walk(wk.dir, guides_walk, &wk);
    if (!wk.found[0]) return NULL;
    FILE* f = re_fopen(wk.found, "rb");
    if (!f) return NULL;
    Guides* gd = g_nguides < 16 ? &g_guides[g_nguides] : &g_guides[hash % 16];
    free(gd->start), free(gd->pts);
    memset(gd, 0, sizeof *gd);
    uint32_t hdr[3];
    bool ok = fread(hdr, sizeof hdr, 1, f) == 1 && hdr[0] == RGRM_MAGIC && hdr[1] == 1u && hdr[2] > 0 && hdr[2] < 1000000u &&
              fread(&gd->info, sizeof gd->info, 1, f) == 1;
    if (ok) { // (a load: two passes over the file, the points' count first)
        long body = ftell(f);
        uint32_t total = 0, n;
        for (uint32_t i = 0; i < hdr[2] && ok; i++) ok = fread(&n, 4, 1, f) == 1 && n >= 2 && n <= 4096 && fseek(f, (long)n * 12, SEEK_CUR) == 0, total += n;
        gd->start = malloc(sizeof(uint32_t) * (hdr[2] + 1)), gd->pts = malloc(sizeof(float) * 3 * (total ? total : 1));
        ok = ok && gd->start && gd->pts && fseek(f, body, SEEK_SET) == 0;
        uint32_t at = 0;
        for (uint32_t i = 0; i < hdr[2] && ok; i++) {
            ok = fread(&n, 4, 1, f) == 1 && fread(gd->pts + at * 3, 12, n, f) == n;
            gd->start[i] = at, at += n;
        }
        if (ok) gd->start[hdr[2]] = at, gd->n = hdr[2], gd->total = at;
    }
    fclose(f);
    if (!ok) {
        free(gd->start), free(gd->pts), memset(gd, 0, sizeof *gd);
        return NULL;
    }
    gd->hash = hash;
    if (g_nguides < 16) g_nguides++;
    printf("[RealEngine] Groom: %u guides read (%s)\n", gd->n, wk.found);
    return gd;
}
// a guide resampled into P points along its length (out: xyz, cm)
static void resample(const Guides* gd, uint32_t i, uint32_t P, float* out) {
    const float* p = gd->pts + gd->start[i] * 3;
    uint32_t n = gd->start[i + 1] - gd->start[i];
    float len = 0.0f, seg[4096];
    for (uint32_t k = 0; k + 1 < n; k++) {
        float dx = p[k * 3 + 3] - p[k * 3], dy = p[k * 3 + 4] - p[k * 3 + 1], dz = p[k * 3 + 5] - p[k * 3 + 2];
        seg[k] = sqrtf(dx * dx + dy * dy + dz * dz), len += seg[k];
    }
    uint32_t k = 0;
    float acc = 0.0f;
    for (uint32_t j = 0; j < P; j++) {
        float want = len * (float)j / (float)(P - 1);
        while (k + 2 < n && acc + seg[k] < want) acc += seg[k], k++;
        float u = seg[k] > 1e-6f ? fminf(fmaxf((want - acc) / seg[k], 0.0f), 1.0f) : 0.0f;
        for (int a = 0; a < 3; a++) out[j * 3 + a] = p[k * 3 + a] + (p[k * 3 + 3 + a] - p[k * 3 + a]) * u;
    }
}

static uint32_t groom_key(const CGroom* g, uint32_t mesh, const float mn[3], const float mx[3]) {
    uint32_t h = 2166136261u;
    h = (h ^ g->strands) * 16777619u, h = (h ^ g->segments) * 16777619u, h = (h ^ g->seed) * 16777619u, h = (h ^ mesh) * 16777619u;
    h = (h ^ (g->dynamic ? 0x5157u : 0u)) * 16777619u; // (2026-11-16: dynamic or not: its simulated points made room for)
    h = (h ^ g->guides) * 16777619u;
    const float f[] = { g->length, g->length_var, g->root_width, g->tip_width, g->curl, g->curl_freq, g->gravity, g->clump,
                        g->scalp, g->hairline, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2] };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) h = hashf(h, f[i]);
    return h ? h : 1u;
}

// Grows a groom over the ellipsoid of its mesh's bounds: guides first (the locks), then every strand pulled toward its
// nearest guide's path; its density grid from its segments (length x width per cm^3)
static bool grow_guides(Grown* o, const CGroom* g, const Guides* gd, float* pts, uint32_t N, uint32_t P);
static bool grow(Grown* o, const CGroom* g, const float mn[3], const float mx[3]) {
    double t0 = re_time_seconds();
    uint32_t N = g->strands < 100 ? 100 : g->strands > 200000 ? 200000 : g->strands;
    uint32_t S = g->segments < 2 ? 2 : g->segments > 32 ? 32 : g->segments, P = S + 1;
    const Guides* gd = guides_get(g->guides); // (2026-11-16: imported guides: the strands from them)
    float c[3], r[3];
    for (int k = 0; k < 3; k++) c[k] = (mn[k] + mx[k]) * 0.5f, r[k] = fmaxf((mx[k] - mn[k]) * 0.5f, 0.5f);
    for (int k = 0; k < 3; k++) o->ell_c[k] = c[k], o->ell_r[k] = r[k]; // (2026-11-16: the simulation keeps the strands out of it)
    o->dyn = g->dynamic != 0, o->sim_reset = true;
    uint32_t NG = N / 80 < 4 ? 4 : N / 80 > 2000 ? 2000 : N / 80;
    float* pts = malloc(sizeof(float) * 4 * P * N);
    float* gpts = malloc(sizeof(float) * 4 * P * NG);
    float* dens = calloc(GROOM_GRID * GROOM_GRID * GROOM_GRID, sizeof(float));
    if (!pts || !gpts || !dens) {
        free(pts), free(gpts), free(dens);
        return false;
    }
    uint32_t s = g->seed * 747796405u + 2891336453u;
    float len0 = g->length > 0.2f ? g->length : 0.2f;
    if (gd) {
        grow_guides(o, g, gd, pts, N, P);
        free(gpts);
        goto grown;
    }
    for (uint32_t i = 0; i < NG; i++) {
        float d[3] = { 0.0f, 0.0f, 1.0f };
        root_dir(&s, g, d);
        chain(g, c, r, d, len0 * (1.0f - g->length_var * 0.5f * rnd(&s)), S, &s, gpts + 4 * P * i);
    }
    float cl = g->clump < 0.0f ? 0.0f : g->clump > 1.0f ? 1.0f : g->clump;
    for (uint32_t i = 0; i < N; i++) {
        float d[3] = { 0.0f, 0.0f, 1.0f };
        root_dir(&s, g, d);
        float* o = pts + 4 * P * i;
        chain(g, c, r, d, len0 * (1.0f - g->length_var * rnd(&s)), S, &s, o);
        if (cl > 0.0f) { // its lock: the nearest guide's path, from this strand's root
            uint32_t best = 0;
            float bd = 1e30f;
            for (uint32_t j = 0; j < NG; j++) {
                const float* q = gpts + 4 * P * j;
                float dx = q[0] - o[0], dy = q[1] - o[1], dz = q[2] - o[2], dd = dx * dx + dy * dy + dz * dz;
                if (dd < bd) bd = dd, best = j;
            }
            const float* q = gpts + 4 * P * best;
            float nn[3];
            for (uint32_t k = 1; k < P; k++) {
                float t = o[4 * k + 3], w = cl * powf(t, 0.7f);
                for (int a = 0; a < 3; a++) {
                    float tgt = q[4 * k + a] + (o[a] - q[a]) * (1.0f - t);
                    o[4 * k + a] += (tgt - o[4 * k + a]) * w;
                }
                keep_out(c, r, o + 4 * k, nn);
            }
        }
    }
    free(gpts);
grown:
    // the box, the density
    float bmin[3] = { 1e30f, 1e30f, 1e30f }, bmax[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < N * P; i++)
        for (int k = 0; k < 3; k++) bmin[k] = fminf(bmin[k], pts[4 * i + k]), bmax[k] = fmaxf(bmax[k], pts[4 * i + k]);
    for (int k = 0; k < 3; k++) bmin[k] -= 1.0f, bmax[k] += 1.0f;
    float cell[3], vol;
    for (int k = 0; k < 3; k++) cell[k] = (bmax[k] - bmin[k]) / GROOM_GRID;
    vol = cell[0] * cell[1] * cell[2];
    float rw = g->root_width * 0.1f, tw = g->tip_width * 0.1f; // (mm -> cm)
    for (uint32_t i = 0; i < N; i++)
        for (uint32_t k = 0; k + 1 < P; k++) {
            const float* a = pts + 4 * (P * i + k);
            const float* b = a + 4;
            float m[3], sl = sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
            int ix[3];
            for (int q = 0; q < 3; q++) {
                m[q] = (a[q] + b[q]) * 0.5f;
                ix[q] = (int)((m[q] - bmin[q]) / cell[q]);
                ix[q] = ix[q] < 0 ? 0 : ix[q] >= GROOM_GRID ? GROOM_GRID - 1 : ix[q];
            }
            float wd = rw + (tw - rw) * (a[3] + b[3]) * 0.5f;
            dens[(ix[2] * GROOM_GRID + ix[1]) * GROOM_GRID + ix[0]] += sl * wd;
        }
    for (int i = 0; i < GROOM_GRID * GROOM_GRID * GROOM_GRID; i++) dens[i] /= vol;
    free(o->pts), free(o->dens);
    o->pts = pts, o->dens = dens, o->strands = N, o->pts_per = P;
    float rad = 0.0f;
    for (int k = 0; k < 3; k++) {
        o->bmin[k] = bmin[k], o->bmax[k] = bmax[k], o->center[k] = (bmin[k] + bmax[k]) * 0.5f;
        rad = fmaxf(rad, (bmax[k] - bmin[k]) * 0.5f);
    }
    o->radius = rad;
    double ms = (re_time_seconds() - t0) * 1000.0;
    G.stats.grow_ms = ms;
    printf("[RealEngine] Groom: %u strands x %u points grown (%.1f ms)\n", N, P, ms);
    return true;
}

// (2026-11-16) The strands from guides: each root near a guide's root (a disk around it as wide as the guides are apart),
// its shape the 3 nearest guides' (weighted by 1 / distance), offset to its root; the clumping pulls it toward its nearest
// guide toward the tip
static bool grow_guides(Grown* o, const CGroom* g, const Guides* gd, float* pts, uint32_t N, uint32_t P) {
    uint32_t NG = gd->n;
    float* gp = malloc(sizeof(float) * 3 * P * NG); // (a grow, not a frame)
    if (!gp) return false;
    for (uint32_t i = 0; i < NG; i++) resample(gd, i, P, gp + 3 * P * i);
    // the spacing: the mean distance from a guide's root to its nearest one
    float spacing = 0.0f;
    for (uint32_t i = 0; i < NG && i < 512; i++) {
        float best = 1e30f;
        for (uint32_t j = 0; j < NG; j++) {
            if (j == i) continue;
            float d = 0.0f;
            for (int a = 0; a < 3; a++) d += (gp[3 * P * i + a] - gp[3 * P * j + a]) * (gp[3 * P * i + a] - gp[3 * P * j + a]);
            best = fminf(best, d);
        }
        spacing += best < 1e29f ? sqrtf(best) : 1.0f;
    }
    spacing = NG > 1 ? spacing / (float)(NG < 512 ? NG : 512) : 1.0f;
    uint32_t s = g->seed * 747796405u + 2891336453u;
    float cl = g->clump < 0.0f ? 0.0f : g->clump > 1.0f ? 1.0f : g->clump;
    for (uint32_t c = 0; c < N; c++) {
        uint32_t base = (uint32_t)(rnd(&s) * (float)NG) % NG;
        const float* b0 = gp + 3 * P * base;
        // a root on a disk around the guide's root, across its first segment
        float t[3] = { b0[3] - b0[0], b0[4] - b0[1], b0[5] - b0[2] }, u[3], v[3];
        norm3(t);
        float up[3] = { 0.0f, 0.0f, 1.0f };
        if (fabsf(t[2]) > 0.9f) up[0] = 1.0f, up[2] = 0.0f;
        u[0] = t[1] * up[2] - t[2] * up[1], u[1] = t[2] * up[0] - t[0] * up[2], u[2] = t[0] * up[1] - t[1] * up[0];
        norm3(u);
        v[0] = t[1] * u[2] - t[2] * u[1], v[1] = t[2] * u[0] - t[0] * u[2], v[2] = t[0] * u[1] - t[1] * u[0];
        float ra = sqrtf(rnd(&s)) * spacing * 0.6f, ph = 6.2831853f * rnd(&s);
        float root[3];
        for (int a = 0; a < 3; a++) root[a] = b0[a] + (u[a] * cosf(ph) + v[a] * sinf(ph)) * ra;
        // its 3 nearest guides by root
        uint32_t nb[3] = { base, base, base };
        float nd[3] = { 1e30f, 1e30f, 1e30f };
        for (uint32_t j = 0; j < NG; j++) {
            const float* q = gp + 3 * P * j;
            float d = (q[0] - root[0]) * (q[0] - root[0]) + (q[1] - root[1]) * (q[1] - root[1]) + (q[2] - root[2]) * (q[2] - root[2]);
            for (int k = 0; k < 3; k++)
                if (d < nd[k]) {
                    for (int m = 2; m > k; m--) nd[m] = nd[m - 1], nb[m] = nb[m - 1];
                    nd[k] = d, nb[k] = j;
                    break;
                }
        }
        float wsum = 0.0f, wk[3];
        for (int k = 0; k < 3; k++) wk[k] = 1.0f / (sqrtf(nd[k]) + 0.1f * spacing), wsum += wk[k];
        float lenf = 1.0f - g->length_var * rnd(&s) * 0.5f;
        float* o4 = pts + 4 * P * c;
        for (uint32_t i = 0; i < P; i++) {
            float acc[3] = { 0, 0, 0 };
            for (int k = 0; k < 3; k++) {
                const float* q = gp + 3 * P * nb[k];
                float li = (float)i * lenf; // (shorter: along its guides less far)
                uint32_t i0 = (uint32_t)li, i1 = i0 + 1 < P ? i0 + 1 : i0;
                float fr = li - (float)i0;
                for (int a = 0; a < 3; a++) acc[a] += (q[3 * i0 + a] + (q[3 * i1 + a] - q[3 * i0 + a]) * fr - q[a]) * wk[k] / wsum;
            }
            float tt = (float)i / (float)(P - 1), w = cl * powf(tt, 0.7f);
            for (int a = 0; a < 3; a++) {
                float mine = root[a] + acc[a], lock = b0[3 * i + a];
                o4[4 * i + a] = mine + (lock - mine) * w * (1.0f - (float)(i == 0));
            }
            o4[4 * i + 3] = tt;
        }
    }
    free(gp);
    return true;
}

// Every groom grown into the staging buffer (the device buffer made bigger when it must; a rare event: the GPU idled)
static bool repack(void) {
    VkDeviceSize need = 0;
    for (uint32_t i = 0; i < G.ng; i++)
        need += (VkDeviceSize)G.g[i].strands * G.g[i].pts_per * 16u * (G.g[i].dyn ? 3u : 1u) + GROOM_GRID * GROOM_GRID * GROOM_GRID * 4u;
    vkDeviceWaitIdle(G.vk->device); // (the frames in flight read the device buffer; an older copy reads the staging one)
    if (need > G.cap) {
        gpu_buf_destroy(G.vk, &G.data), gpu_buf_destroy(G.vk, &G.staging);
        VkDeviceSize cap = need + need / 2 + 65536u;
        if (!gpu_buf_create(G.vk, &G.data, cap, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT, false) ||
            !gpu_buf_create(G.vk, &G.staging, cap, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, true)) {
            gpu_buf_destroy(G.vk, &G.data), gpu_buf_destroy(G.vk, &G.staging);
            G.cap = 0;
            printf("[RealEngine] aviso: o cabelo nao conseguiu %.1f MB\n", (double)cap / 1048576.0);
            return false;
        }
        G.cap = cap;
    }
    uint8_t* dst = G.staging.mapped;
    VkDeviceSize at = 0;
    for (uint32_t i = 0; i < G.ng; i++) {
        Grown* g = &G.g[i];
        VkDeviceSize pb = (VkDeviceSize)g->strands * g->pts_per * 16u, db = GROOM_GRID * GROOM_GRID * GROOM_GRID * 4u;
        memcpy(dst + at, g->pts, pb);
        g->first = (uint32_t)(at / 16u);
        at += pb;
        memcpy(dst + at, g->dens, db);
        g->dens_first = (uint32_t)(at / 4u);
        at += db;
    }
    G.used = at;
    G.upload = at > 0;
    for (uint32_t i = 0; i < G.ng; i++) { // (2026-11-16) the dynamic ones' simulated points past what is uploaded (the GPU's own)
        Grown* g = &G.g[i];
        if (!g->dyn) continue;
        VkDeviceSize pb = (VkDeviceSize)g->strands * g->pts_per * 16u;
        g->sim_cur = (uint32_t)(at / 16u), at += pb;
        g->sim_prev = (uint32_t)(at / 16u), at += pb;
        g->sim_reset = true;
    }
    return true;
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

// (2026-11-16) A dynamic groom's step: its rows, the head, the bones near it (the actor's Skeletal Mesh: capsules from each
// bone to its child, the Physics Asset's radius where it has one), the scene's boxes around it
static void sim_setup(World* w, Grown* g, const CGroom* c, const float rows[3][4], Entity e) {
    if (G.nsim >= GROOM_MAX) return;
    HairSimGpu* s = &((HairSimGpu*)G.simp[G.slot].mapped)[G.nsim];
    memset(s, 0, sizeof *s);
    memcpy(s->rows, rows, sizeof s->rows);
    inverse_rows(rows, s->inv);
    s->rest = g->first, s->cur = g->sim_cur, s->prev = g->sim_prev, s->strands = g->strands, s->pts = g->pts_per;
    s->reset = g->sim_reset ? 1u : 0u, g->sim_reset = false;
    s->dt = G.dt;
    s->stiff = c->stiffness < 0.0f ? 0.0f : c->stiffness > 1.0f ? 1.0f : c->stiffness;
    s->damp = c->damping < 0.0f ? 0.0f : c->damping > 0.9f ? 0.9f : c->damping;
    s->grav = c->gravity > 0.0f ? 1.0f : 0.0f; // (Gravity shapes the grown hair; the simulated one always falls)
    for (int k = 0; k < 3; k++) s->wind[k] = c->wind[k];
    s->wind[3] = c->gusts < 0.0f ? 0.0f : c->gusts > 1.0f ? 1.0f : c->gusts;
    for (int k = 0; k < 3; k++) s->ell_c[k] = g->ell_c[k], s->ell_r[k] = g->ell_r[k];
    s->ell_r[3] = c->collision_radius > 0.0f ? c->collision_radius : 0.0f;
    if (!c->collide) s->ell_r[0] = s->ell_r[1] = s->ell_r[2] = 1e-3f; // (no head either: a dot)
    // the world box around it (cm): the hair's reach
    float cw[3], reach = g->radius * 1.5f + 30.0f;
    for (int i = 0; i < 3; i++) cw[i] = rows[i][0] * g->center[0] + rows[i][1] * g->center[1] + rows[i][2] * g->center[2] + rows[i][3];
    if (c->collide) {
        // the bones: the actor's own Skeletal Mesh (or its owner's: a groom component on a character)
        Entity owner = e;
        const CAttach* at = ecs_get(w, e, COMP_ATTACH);
        if (at && at->owner && ecs_alive(w, at->owner)) owner = at->owner;
        Entity sk = anim_mesh_entity(w, owner);
        const CSkeletalMesh* skc = sk ? ecs_get(w, sk, COMP_SKELETAL_MESH) : NULL;
        const CTransform* skt = sk ? ecs_get(w, sk, COMP_TRANSFORM) : NULL;
        const SkelMesh* m = skc ? anim_mesh(skc->mesh) : NULL;
        uint32_t nb = 0;
        const float (*pal)[3][4] = m ? anim_skin_palette(sk, &nb, NULL) : NULL;
        const PhysicsAsset* pa = skc && skc->physics_asset ? phys_find(skc->physics_asset) : NULL;
        if (pal && skt) {
            float mr[3][4];
            render_transform_rows(skt, mr);
            for (uint32_t b = 0; b < m->nbones && b < nb && s->ncap < 32; b++) {
                int child = -1;
                for (uint32_t q = b + 1; q < m->nbones && child < 0; q++)
                    if (m->bones[q].parent == (int16_t)b) child = (int)q;
                if (child < 0 || (uint32_t)child >= nb) continue;
                float pts[2][3];
                for (int e2 = 0; e2 < 2; e2++) { // the bone's head (bind) through the palette (mesh space), into the world
                    int bb = e2 ? child : (int)b;
                    const float* hb = &m->bind_world[bb][0][3];
                    float hp[3] = { m->bind_world[bb][0][3], m->bind_world[bb][1][3], m->bind_world[bb][2][3] }, mp[3];
                    (void)hb;
                    for (int i = 0; i < 3; i++) mp[i] = pal[b][i][0] * hp[0] + pal[b][i][1] * hp[1] + pal[b][i][2] * hp[2] + pal[b][i][3];
                    if (e2) for (int i = 0; i < 3; i++) mp[i] = pal[child][i][0] * hp[0] + pal[child][i][1] * hp[1] + pal[child][i][2] * hp[2] + pal[child][i][3];
                    for (int i = 0; i < 3; i++) pts[e2][i] = mr[i][0] * mp[0] + mr[i][1] * mp[1] + mr[i][2] * mp[2] + mr[i][3];
                }
                float mid[3], len2 = 0.0f, d2 = 0.0f;
                for (int i = 0; i < 3; i++) mid[i] = (pts[0][i] + pts[1][i]) * 0.5f, len2 += (pts[1][i] - pts[0][i]) * (pts[1][i] - pts[0][i]), d2 += (mid[i] - cw[i]) * (mid[i] - cw[i]);
                float len = sqrtf(len2);
                if (len < 2.0f || sqrtf(d2) > reach + len) continue; // (a tiny bone; one far from the hair)
                float rad = fminf(fmaxf(len * 0.18f, 2.0f), 10.0f);
                for (uint32_t pb = 0; pa && pb < pa->nbodies; pb++)
                    if (!strcmp(pa->bodies[pb].bone, m->bones[b].name)) rad = fmaxf(pa->bodies[pb].radius, 1.0f);
                float* cp = s->caps[s->ncap++];
                cp[0] = pts[0][0], cp[1] = pts[0][1], cp[2] = pts[0][2], cp[3] = rad;
                cp[4] = pts[1][0], cp[5] = pts[1][1], cp[6] = pts[1][2], cp[7] = 0.0f;
            }
        }
        // the scene's boxes (the play's collision grid)
        float mn[3], mx[3], bx[16][6];
        for (int i = 0; i < 3; i++) mn[i] = cw[i] - reach, mx[i] = cw[i] + reach;
        uint32_t nbx = bp_world_boxes(mn, mx, bx, 16);
        for (uint32_t i = 0; i < nbx; i++) {
            float* bp = s->boxes[s->nbox++];
            bp[0] = bx[i][0], bp[1] = bx[i][1], bp[2] = bx[i][2], bp[4] = bx[i][3], bp[5] = bx[i][4], bp[6] = bx[i][5];
        }
    }
    G.sim_grown[G.nsim++] = (uint32_t)(g - G.g);
}

void groom_fill(World* w, const float cam[3], float fov_deg, uint32_t width_px, uint32_t slot) {
    G.slot = slot % RE_FRAMES_IN_FLIGHT;
    G.ndraw = 0, G.moving = false;
    G.stats.grooms = G.stats.strands = G.stats.drawn = G.stats.points = 0;
    if (!G.vk || getenv("REALENGINE_NO_HAIR")) return; // (measuring without)
    for (uint32_t i = 0; i < G.ng; i++) G.g[i].used = false;
    { // (2026-11-16) the simulation's step: this frame's time (a long pause: one 30th)
        double now = re_time_seconds();
        G.dt = G.last_t > 0.0 ? (float)fmin(fmax(now - G.last_t, 1e-4), 1.0 / 30.0) : 1.0f / 60.0f;
        G.last_t = now, G.nsim = 0;
    }
    struct { uint32_t grown; float rows[3][4]; const CGroom* c; Entity e; } todo[GROOM_MAX];
    uint32_t nt = 0;
    bool changed = false;
    for (uint32_t ai = 0; ai < w->archetype_count && nt < GROOM_MAX; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_GROOM)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CGroom* gc = ecs_arch_column(a, COMP_GROOM);
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CStaticMesh* sm = (a->mask & COMP_BIT(COMP_STATIC_MESH)) ? ecs_arch_column(a, COMP_STATIC_MESH) : NULL;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t r = 0; r < a->count && nt < GROOM_MAX; r++) {
            float mn[3] = { -50.0f, -50.0f, -50.0f }, mx[3] = { 50.0f, 50.0f, 50.0f }; // (no mesh: the engine's 1 m sphere)
            uint32_t mesh = sm ? sm[r].mesh : 0;
            if (sm) render_mesh_bounds(mesh, mn, mx);
            uint32_t key = groom_key(&gc[r], mesh, mn, mx);
            Grown* g = NULL;
            for (uint32_t i = 0; i < G.ng && !g; i++)
                if (G.g[i].e == ents[r]) g = &G.g[i];
            if (!g && G.ng < GROOM_MAX) g = &G.g[G.ng++], memset(g, 0, sizeof *g), g->e = ents[r];
            if (!g) continue;
            g->used = true;
            if (g->key != key) {
                g->key = key;
                if (!grow(g, &gc[r], mn, mx)) continue;
                changed = true;
            }
            if (!g->pts) continue;
            todo[nt].grown = (uint32_t)(g - G.g), todo[nt].c = &gc[r], todo[nt].e = ents[r];
            render_transform_rows(&tr[r], todo[nt].rows);
            nt++;
        }
    }
    for (uint32_t i = 0; i < G.ng;) { // (the grooms gone: their room given back)
        if (!G.g[i].used) {
            Entity e = G.g[i].e;
            (void)e;
            grown_free(&G.g[i]);
            G.g[i] = G.g[--G.ng];
            memset(&G.g[G.ng], 0, sizeof G.g[G.ng]);
            for (uint32_t k = 0; k < nt; k++)
                if (todo[k].grown == G.ng) todo[k].grown = i;
            changed = true;
        } else
            i++;
    }
    if (changed && !repack()) return;
    if (!G.data.buffer) return;
    GpuGroom* out = G.info[G.slot].mapped;
    float tan_half = tanf(fmaxf(fov_deg, 1.0f) * 0.5f * 3.14159265f / 180.0f);
    // (Q27.3) each groom's wish (its strands by its size on the screen) and its distance; the segments over the budget:
    // every wish scaled down by the same factor (never under 4% of its strands: the far ones stay a head of hair); the
    // nearest HAIR_SIM_MAX dynamic ones simulated
    static uint32_t wish[GROOM_MAX];
    static float gdist[GROOM_MAX];
    static uint8_t may_sim[GROOM_MAX];
    uint64_t want_seg = 0;
    for (uint32_t k = 0; k < nt; k++) {
        const Grown* g = &G.g[todo[k].grown];
        float wc[3], sc = 0.0f;
        for (int i = 0; i < 3; i++) {
            wc[i] = todo[k].rows[i][0] * g->center[0] + todo[k].rows[i][1] * g->center[1] + todo[k].rows[i][2] * g->center[2] + todo[k].rows[i][3];
            float al = sqrtf(todo[k].rows[0][i] * todo[k].rows[0][i] + todo[k].rows[1][i] * todo[k].rows[1][i] + todo[k].rows[2][i] * todo[k].rows[2][i]);
            sc = fmaxf(sc, al);
        }
        float dx = wc[0] - cam[0], dy = wc[1] - cam[1], dz = wc[2] - cam[2];
        float dist = sqrtf(dx * dx + dy * dy + dz * dz), px = g->radius * sc / fmaxf(dist * tan_half, 1e-3f) * (float)width_px * 0.5f;
        float frac = todo[k].c->lod ? fminf(fmaxf(px / 350.0f, 0.06f), 1.0f) : 1.0f;
        wish[k] = (uint32_t)((float)g->strands * frac + 0.5f);
        if (wish[k] < 1) wish[k] = 1;
        gdist[k] = dist, may_sim[k] = 0;
        want_seg += (uint64_t)wish[k] * (g->pts_per > 1 ? g->pts_per - 1u : 1u);
    }
    float budget_k = want_seg > HAIR_BUDGET_SEGMENTS ? (float)HAIR_BUDGET_SEGMENTS / (float)want_seg : 1.0f;
    bool no_budget = getenv("REALENGINE_HAIR_NO_BUDGET") != NULL; // (a measure: as before, every one simulated, no budget)
    if (no_budget) budget_k = 1.0f;
    for (uint32_t n = 0; n < (no_budget ? GROOM_MAX : HAIR_SIM_MAX); n++) { // (the nearest dynamic ones not picked yet)
        int best = -1;
        for (uint32_t k = 0; k < nt; k++)
            if (todo[k].c->dynamic && !may_sim[k] && (best < 0 || gdist[k] < gdist[best])) best = (int)k;
        if (best < 0) break;
        may_sim[best] = 1;
    }
    G.stats.budget_k = budget_k, G.stats.simulated = 0, G.stats.segments = 0;
    for (uint32_t k = 0; k < nt; k++) {
        const Grown* g = &G.g[todo[k].grown];
        const CGroom* c = todo[k].c;
        GpuGroom* o = &out[G.ndraw];
        memset(o, 0, sizeof *o);
        memcpy(o->rows, todo[k].rows, sizeof o->rows);
        inverse_rows(todo[k].rows, o->inv);
        { // (2026-11-15) last frame's rows: what moved draws its motion in the velocity pass
            Grown* gm = &G.g[todo[k].grown];
            memcpy(o->prev, gm->has_prev ? gm->prev_rows : todo[k].rows, sizeof o->prev);
            if (gm->has_prev && memcmp(gm->prev_rows, todo[k].rows, sizeof gm->prev_rows) != 0) G.moving = true;
            float tx = todo[k].rows[0][3] - gm->prev_rows[0][3], ty = todo[k].rows[1][3] - gm->prev_rows[1][3], tz = todo[k].rows[2][3] - gm->prev_rows[2][3];
            if (gm->has_prev && tx * tx + ty * ty + tz * tz > 100.0f * 100.0f) gm->sim_reset = true; // (2026-11-16: a teleport: the hair starts over there)
            memcpy(gm->prev_rows, todo[k].rows, sizeof gm->prev_rows), gm->has_prev = true;
        }
        // the LOD: its wish (its size on the screen: under 350 px fewer strands, the rest wider) times the budget's factor
        uint32_t drawn = (uint32_t)((float)wish[k] * budget_k + 0.5f), floor_n = (uint32_t)((float)g->strands * 0.04f);
        if (drawn < floor_n) drawn = floor_n < wish[k] ? floor_n : wish[k];
        if (drawn < 1) drawn = 1;
        o->first = g->first, o->strands = g->strands, o->pts = g->pts_per, o->drawn = drawn;
        o->root_w = c->root_width * 0.1f, o->tip_w = c->tip_width * 0.1f, o->cover = (float)g->strands / (float)drawn;
        o->ao = c->occlusion < 0.0f ? 0.0f : c->occlusion > 1.0f ? 1.0f : c->occlusion;
        // the fiber's color (Chiang et al. 2016's absorption by eumelanin and pheomelanin), through a path of a fiber's
        // width (Karis's base color), tinted by the dye
        float m = c->melanin < 0.0f ? 0.0f : c->melanin > 1.0f ? 1.0f : c->melanin, rd = c->redness < 0.0f ? 0.0f : c->redness > 1.0f ? 1.0f : c->redness;
        float conc = 8.0f * m * m, eu = conc * (1.0f - rd), ph = conc * rd + 0.08f * rd;
        static const float ea[3] = { 0.419f, 0.697f, 1.37f }, pa[3] = { 0.187f, 0.4f, 1.05f };
        for (int i = 0; i < 3; i++) {
            float dye = c->dye[i] < 0.0f ? 0.0f : c->dye[i] > 1.0f ? 1.0f : c->dye[i];
            o->color[i] = expf(-(eu * ea[i] + ph * pa[i])) * dye;
        }
        o->color[3] = c->roughness;
        for (int i = 0; i < 3; i++) o->bmin[i] = g->bmin[i], o->bmax[i] = g->bmax[i];
        o->bmin[3] = (float)g->dens_first; // (exact: a float's 24 bits hold up to 16 M floats, 64 MB of hair)
        if (g->dyn && !may_sim[k]) G.g[todo[k].grown].sim_reset = true; // (Q27.3: past the nearest: as grown; back near, it starts over)
        if (g->dyn && g->sim_cur && G.sim_pipe && may_sim[k]) { // (2026-11-16) simulated: its world points drawn, its step set up
            sim_setup(w, &G.g[todo[k].grown], c, todo[k].rows, todo[k].e);
            static const float id[3][4] = { { 1, 0, 0, 0 }, { 0, 1, 0, 0 }, { 0, 0, 1, 0 } };
            memcpy(o->rows, id, sizeof o->rows), memcpy(o->prev, id, sizeof o->prev); // (inv stays the actor's: the shadow grid)
            o->first = g->sim_cur, o->dyn = 1, o->prev_first = g->sim_prev;
            G.moving = true;
        }
        G.verts[G.ndraw] = drawn * (g->pts_per - 1u) * 6u;
        G.stats.grooms++, G.stats.strands += g->strands, G.stats.drawn += drawn, G.stats.points += g->strands * g->pts_per;
        G.stats.segments += drawn * (g->pts_per > 1 ? g->pts_per - 1u : 1u), G.stats.simulated += o->dyn != 0;
        G.ndraw++;
    }
}

static void groom_upload(VkCommandBuffer cmd);
void groom_record(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout) {
    groom_upload(cmd);
    if (!G.nsim || !G.sim_pipe || !G.data.buffer) return;
    // (2026-11-16) the dynamic grooms' step: last frame's draws read the points it moves
    VkMemoryBarrier2 b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    b.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_COPY_BIT, b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    gpu_mark(cmd, "cabelo (simulacao)");
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, G.sim_pipe);
    Push p = *push;
    p.vertices = G.data.addr;
    p.pad = (uint32_t)(fmod(re_time_seconds(), 3600.0) * 1000.0);
    for (uint32_t i = 0; i < G.nsim; i++) {
        p.instances = G.simp[G.slot].addr + sizeof(HairSimGpu) * i, p.pass = i;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vkCmdDispatch(cmd, (G.g[G.sim_grown[i]].strands + 63u) / 64u, 1, 1);
    }
    b.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, b.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &di);
}

static void groom_upload(VkCommandBuffer cmd) {
    if (!G.upload || !G.data.buffer || !G.used) return;
    VkMemoryBarrier2 b = { VK_STRUCTURE_TYPE_MEMORY_BARRIER_2 };
    b.srcStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
    b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    VkDependencyInfo di = { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
    di.memoryBarrierCount = 1, di.pMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &di);
    VkBufferCopy rg = { 0, 0, G.used };
    vkCmdCopyBuffer(cmd, G.staging.buffer, G.data.buffer, 1, &rg);
    b.srcStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, b.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_READ_BIT;
    vkCmdPipelineBarrier2(cmd, &di);
    G.upload = false;
}

bool groom_moving(void) { return G.moving && G.ndraw; }
uint32_t groom_draw_velocity(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline pipe) {
    if (!G.ndraw || !pipe || !G.data.buffer || !G.moving) return 0;
    Push p = *push;
    p.instances = G.info[G.slot].addr;
    p.vertices = G.data.addr;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    for (uint32_t i = 0; i < G.ndraw; i++) {
        p.pass = i;
        vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
        vkCmdDraw(cmd, G.verts[i], 1, 0, 0);
    }
    return G.ndraw;
}
uint32_t groom_draw(VkCommandBuffer cmd, const Push* push, VkPipelineLayout layout, VkPipeline depth, VkPipeline pipe) {
    if (!G.ndraw || !pipe || !depth || !G.data.buffer) return 0;
    Push p = *push;
    p.instances = G.info[G.slot].addr;
    p.vertices = G.data.addr;
    for (int pass = 0; pass < 2; pass++) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pass ? pipe : depth);
        for (uint32_t i = 0; i < G.ndraw; i++) {
            p.pass = i;
            vkCmdPushConstants(cmd, layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT | VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof p, &p);
            vkCmdDraw(cmd, G.verts[i], 1, 0, 0);
        }
    }
    return 2u * G.ndraw;
}
