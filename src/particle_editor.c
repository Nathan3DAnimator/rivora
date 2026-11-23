// The Particle System editor (particles.h), laid out like Unreal's Niagara Editor:
//   - the toolbar: Save, Browse (to the asset), Play / Pause, Restart, the speed, Preview in Level;
//   - Preview (left): the system playing on its own, orbited with the mouse (a CPU sample of its particles: the same
//     births, shapes, speeds, forces, colors and sizes as particles.glsl; the exact look is Preview in Level);
//   - System Overview (center): the System node and one node per emitter, its stack in Niagara's colored groups
//     (Emitter, Particle Spawn, Particle Update, Render), each module with its checkbox; "+" adds one to a group;
//   - Selection (right): the selected emitter's stack, each module unfolded with its parameters (vectors, colors,
//     choices, toggles as such), the selected one open;
//   - Curves / Timeline (bottom): Color Over Life as a gradient and Size Over Life as a curve, dragged with the mouse;
//     every emitter's spawning over time, the playhead dragged to scrub the preview.
// 2026-11-08: rewritten from the three plain lists (whose modules showed only their checkbox: a negative width hid the
// names), at the user's request ("make it like Niagara, in what it can do and in how the editor looks").
#include "particle_editor.h"
#include "asset_viewport.h"
#include "bp_preview.h"

#include "theme.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "components.h"
#include "content.h"
#include "editor.h"
#include "graph_menu.h"
#include "graph_style.h"
#include "i18n.h"
#include "material.h"
#include "particles.h"
#include "render.h"
#include "render_gpu.h"
#include "ui_icons.h"
#include "ui_widget.h"

#define PREVIEW_MAX 500 // particles sampled per emitter in the preview
#define SEL_SYSTEM -2   // E.emitter: the System node

static struct {
    ParticleSystemAsset* a;
    int emitter, module;
    Entity preview;
    bool unsaved;
    char status[96];
    // the preview
    bool paused;
    float t, speed;
    float yaw, pitch, dist, focus_z;
    int open_module; // a module clicked in the overview: unfolded (and scrolled to) in Selection next frame
    int bottom_tab;
    AssetViewport vp; // (Q7) the preview's scene: the asset viewport (floor, sky, light, camera)
    int shown;        // the particles drawn last frame
} E = { NULL, 0, -1, ENTITY_NONE, false, "", false, 0.0f, 1.0f, 225.0f, 18.0f, 700.0f, 120.0f, -1, 0, { 0 }, 0 };

int particle_editor_preview_count(void) { return E.a ? E.shown : 0; }

static const char* const g_stage_names[PSS_COUNT] = { "Emitter", "Particle Spawn", "Particle Update", "Render" };
// Niagara's group colors: the emitter's red-orange, the particles' green, the renderers' purple
static const uint32_t g_stage_colors[PSS_COUNT] = { TK_ORANGE, TK_GREEN, TK_LIME, TK_VIOLET };
#define k_system_color TH(TK_ORANGE) // (the System node: orange)

void particle_editor_open(const char* vpath) {
    if (content_defer_open(vpath)) return;
    E.a = ps_load(vpath);
    E.emitter = 0, E.module = -1, E.unsaved = false, E.status[0] = 0;
    E.t = 0.0f, E.paused = false, E.open_module = -1;
    const char* sel = getenv("REALENGINE_PE_SELECT"); // (screenshots: "<emitter>,<module>" selected and unfolded)
    if (sel && E.a) sscanf(sel, "%d,%d", &E.emitter, &E.module), E.open_module = E.module;
}

bool particle_editor_handles(const char* vpath) {
    const char* dot = strrchr(vpath, '.');
    return dot && !_stricmp(dot, ".rparticle");
}

bool particle_editor_add_menu(const char* folder_vpath) {
    bool ok = !strncmp(folder_vpath, "/Content", 8), made = false;
    if (!ui_begin_menu(ICON_BULB, TR("Visual Effects"), ok)) return false;
    igTextDisabled("%s", TR("Particle System from a template"));
    for (int t = 0; t < PST_COUNT; t++) {
        if (!ui_menu_item(ICON_BULB, TR(g_ps_template_names[t]), NULL, false, true)) continue;
        char name[64];
        snprintf(name, sizeof name, "PS_%s", g_ps_template_names[t]);
        for (char* c = name; *c; c++)
            if (*c == ' ' || *c == '(' || *c == ')') *c = '_';
        uint32_t h = ps_create(folder_vpath, name, t);
        if (h) {
            char vp[700];
            snprintf(vp, sizeof vp, "%s/%s.rparticle", folder_vpath, name);
            particle_editor_open(vp);
            made = true;
        }
    }
    igEndMenu();
    return made;
}

// The preview in the level: an actor playing the asset in front of the camera (removed when the window closes)
static void preview(Editor* ed, bool on) {
    World* w = &ed->world;
    if (!on) {
        if (E.preview != ENTITY_NONE && ecs_alive(w, E.preview)) ecs_destroy(w, E.preview);
        E.preview = ENTITY_NONE;
        return;
    }
    if (E.preview != ENTITY_NONE && ecs_alive(w, E.preview)) return;
    float cy = cosf(ed->cam.yaw * 0.017453293f), sy = sinf(ed->cam.yaw * 0.017453293f);
    float at[3] = { ed->cam.pos[0] + cy * 700.0f, ed->cam.pos[1] + sy * 700.0f, fmaxf(ed->cam.pos[2] - 120.0f, 0.0f) };
    E.preview = particles_spawn(w, E.a->hash, at, NULL, false);
    if (E.preview != ENTITY_NONE) snprintf(((CName*)ecs_get(w, E.preview, COMP_NAME))->text, RE_NAME_MAX, "ParticlePreview");
}

// ---------------------------------------------------------------------------------------------------------------------
// The CPU preview: particles.glsl's births, randoms and spawn shapes on a sample of the slots, the forces integrated
// from each birth (1/30 s steps), drawn as discs in the Preview panel.

static uint32_t p_hash(uint32_t x) {
    x ^= x >> 16, x *= THA(TK_BLUE, 127), x ^= x >> 15, x *= THA(TK_LIME, 132), x ^= x >> 16;
    return x;
}
static void p_rand4(uint32_t seed, uint32_t slot, uint32_t k, float r[4]) {
    uint32_t h = p_hash(seed ^ p_hash(slot * 0x9E3779B9u ^ p_hash(k + THA(TK_MAGENTA, 104))));
    uint32_t q[4] = { h, p_hash(h + 1u), p_hash(h + 2u), p_hash(h + 3u) };
    for (int i = 0; i < 4; i++) r[i] = (float)(q[i] >> 8u) * (1.0f / 16777216.0f);
}

static bool p_birth(const GpuEmitter* e, uint32_t i, float t, float* b, uint32_t* k) {
    *b = 0.0f, *k = 0u;
    if (i < e->rate_slots) {
        if (e->rate <= 0.0f) return false;
        float b0 = (float)i / e->rate, period = (float)e->rate_slots / e->rate, last = fminf(t, e->stop);
        if (last < b0) return false;
        float kk = floorf((last - b0) / period);
        *b = b0 + kk * period, *k = (uint32_t)kk;
        return true;
    }
    if (t < e->burst_time || (float)(i - e->rate_slots) >= e->burst_count) return false;
    float kk = e->burst_interval > 0.0f ? floorf((fminf(t, e->stop) - e->burst_time) / e->burst_interval) : 0.0f;
    if (kk < 0.0f) return false;
    *b = e->burst_time + kk * e->burst_interval, *k = (uint32_t)kk;
    return *b <= e->stop;
}

static void v3_norm(float v[3]) {
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) v[0] /= l, v[1] /= l, v[2] /= l;
}

static void spawn_shape(const GpuEmitter* e, const float r[4], const float r2[4], float p[3], float out[3]) {
    uint32_t s = e->shape_kind;
    const float* sp = e->shape_p;
    p[0] = p[1] = p[2] = 0.0f, out[0] = out[1] = 0.0f, out[2] = 1.0f;
    float tau = 6.2831853f;
    if (s == 1u || s >= 5u && s <= 7u) { // a sphere (a mesh's shape: a sphere of 50 cm, the preview has no mesh)
        float rad = s == 1u ? sp[0] : 50.0f;
        float z = r[0] * 2.0f - 1.0f, phi = r[1] * tau, rr = sqrtf(fmaxf(1.0f - z * z, 0.0f));
        out[0] = rr * cosf(phi), out[1] = rr * sinf(phi), out[2] = z;
        float d = (s == 1u && sp[1] > 0.5f) ? rad : rad * cbrtf(r[2]);
        for (int i = 0; i < 3; i++) p[i] = out[i] * d;
    } else if (s == 2u) {
        for (int i = 0; i < 3; i++) p[i] = (r[i] * 2.0f - 1.0f) * sp[i], out[i] = p[i] + 1e-3f;
        v3_norm(out);
    } else if (s == 3u) {
        float a = sp[0] * 0.017453293f * sqrtf(r[0]), phi = r[1] * tau;
        out[0] = sinf(a) * cosf(phi), out[1] = sinf(a) * sinf(phi), out[2] = cosf(a);
        for (int i = 0; i < 3; i++) p[i] = out[i] * sp[1] * r[2];
    } else if (s == 4u) {
        float rad = sp[0] * sqrtf(r[0]), phi = r[1] * tau;
        p[0] = rad * cosf(phi), p[1] = rad * sinf(phi);
    } else if (s == 8u) {
        float rad = sp[0] * (sp[2] > 0.5f ? 1.0f : sqrtf(r[0])), phi = r[1] * tau;
        out[0] = cosf(phi), out[1] = sinf(phi), out[2] = 0.0f;
        p[0] = out[0] * rad, p[1] = out[1] * rad, p[2] = sp[1] * r[2];
    } else if (s == 9u) {
        float phi = r[1] * tau, th = r[2] * tau, rr = sp[1] * (sp[2] > 0.5f ? 1.0f : sqrtf(r[0]));
        float ring[3] = { cosf(phi), sinf(phi), 0.0f };
        for (int i = 0; i < 3; i++) out[i] = ring[i] * cosf(th) + (i == 2 ? sinf(th) : 0.0f);
        for (int i = 0; i < 3; i++) p[i] = ring[i] * sp[0] + out[i] * rr;
    }
    (void)r2;
}

typedef struct PrevPart {
    float p[3], size, col[4];
} PrevPart;

static float life_of(const GpuEmitter* e, const float r[4]) { return e->life_min + (e->life_max - e->life_min) * r[0]; }

static void color_of(const GpuEmitter* e, const float r[4], float a, float c[4]) {
    for (int i = 0; i < 4; i++) c[i] = e->color0[i] + (e->color1[i] - e->color0[i]) * r[1];
    if (!(e->flags & PF_COLOR_LIFE)) return;
    a = fminf(fmaxf(a, 0.0f), 1.0f);
    const float* t = e->col_life_t;
    int s = a <= t[1] ? 0 : a <= t[2] ? 1 : 2;
    float f = fminf(fmaxf((a - t[s]) / fmaxf(t[s + 1] - t[s], 1e-4f), 0.0f), 1.0f);
    for (int i = 0; i < 4; i++) c[i] *= e->col_life[s][i] + (e->col_life[s + 1][i] - e->col_life[s][i]) * f;
}

static float size_of(const GpuEmitter* e, const float r[4], float a, const float v[3]) {
    float f = 1.0f;
    if (e->flags & PF_SIZE_LIFE) {
        float x = fminf(fmaxf(a, 0.0f), 1.0f) * 3.0f;
        int s = x >= 2.0f ? 2 : (int)x;
        f = e->size_life[s] + (e->size_life[s + 1] - e->size_life[s]) * (x - (float)s);
    }
    if (e->flags & PF_SPEED_SCALE) {
        float sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
        float u = fminf(fmaxf((sp - e->speed_scale[0]) / fmaxf(e->speed_scale[1] - e->speed_scale[0], 1e-3f), 0.0f), 1.0f);
        f *= e->speed_scale[2] + (e->speed_scale[3] - e->speed_scale[2]) * u;
    }
    return (e->size_p[0] + (e->size_p[1] - e->size_p[0]) * r[2]) * f;
}

// One emitter's sample at time t (the emitter's own: after its start delay); returns how many
static int simulate(const GpuEmitter* e, float t, PrevPart* out, int max) {
    int n = 0;
    if (!e->capacity || t <= 0.0f) return 0;
    uint32_t stride = e->capacity > (uint32_t)max ? (e->capacity + (uint32_t)max - 1u) / (uint32_t)max : 1u;
    for (uint32_t i = 0; i < e->capacity && n < max; i += stride) {
        float b;
        uint32_t k;
        if (!p_birth(e, i, t, &b, &k)) continue;
        float r[4], r2[4], r3[4];
        p_rand4(e->seed, i, k, r), p_rand4(e->seed ^ 0xA511E9B3u, i, k, r2), p_rand4(e->seed ^ 0x63D83595u, i, k, r3);
        float life = life_of(e, r), age = t - b;
        if (age < 0.0f || age >= life) continue;
        float p[3], o[3];
        spawn_shape(e, r2, r3, p, o);
        // the velocity: a cone around the direction, less up to the random part, outward
        float d[3] = { e->vel_dir[0], e->vel_dir[1], e->vel_dir[2] + 1e-6f };
        v3_norm(d);
        float ca = e->vel_p[0] * 0.017453293f * sqrtf(r3[0]), phi = r3[1] * 6.2831853f;
        float tx[3], up[3] = { 0, 0, 1 }, xx[3] = { 1, 0, 0 };
        const float* ref = fabsf(d[2]) < 0.99f ? up : xx;
        tx[0] = d[1] * ref[2] - d[2] * ref[1], tx[1] = d[2] * ref[0] - d[0] * ref[2], tx[2] = d[0] * ref[1] - d[1] * ref[0];
        v3_norm(tx);
        float ty[3] = { d[1] * tx[2] - d[2] * tx[1], d[2] * tx[0] - d[0] * tx[2], d[0] * tx[1] - d[1] * tx[0] };
        float v[3];
        for (int c = 0; c < 3; c++) {
            float dir = d[c] * cosf(ca) + (tx[c] * cosf(phi) + ty[c] * sinf(phi)) * sinf(ca);
            v[c] = dir * e->vel_dir[3] * (1.0f - e->vel_p[1] * r3[2]) + o[c] * e->vel_p[2];
        }
        // the forces, from the birth to now
        int steps = (int)ceilf(age * 30.0f);
        if (steps > 45) steps = 45;
        float dt = steps ? age / (float)steps : 0.0f;
        bool dead = false;
        for (int s = 0; s < steps && !dead; s++) {
            float a[3] = { 0, 0, 0 };
            if (e->flags & PF_GRAVITY)
                for (int c = 0; c < 3; c++) a[c] += e->gravity[c];
            if (e->flags & PF_ATTRACT) {
                float rel[3] = { e->attract[0] - p[0], e->attract[1] - p[1], e->attract[2] - p[2] };
                float l = sqrtf(rel[0] * rel[0] + rel[1] * rel[1] + rel[2] * rel[2]);
                if (e->attract2[1] > 0.0f && l < e->attract2[1]) dead = true;
                if (l > 1e-3f && (e->attract2[0] <= 0.0f || l < e->attract2[0]))
                    for (int c = 0; c < 3; c++) a[c] += rel[c] / l * e->attract[3];
            }
            if (e->flags & PF_VORTEX) { // pulled toward the Z axis, turned around it
                float rl = sqrtf(p[0] * p[0] + p[1] * p[1]);
                if (rl > 1e-3f) a[0] -= p[0] / rl * e->vortex[1], a[1] -= p[1] / rl * e->vortex[1];
            }
            for (int c = 0; c < 3; c++) v[c] += a[c] * dt;
            if (e->flags & PF_DRAG)
                for (int c = 0; c < 3; c++) v[c] *= fmaxf(1.0f - e->gravity[3] * dt, 0.0f);
            if (e->flags & PF_WIND)
                for (int c = 0; c < 3; c++) v[c] += (e->wind[c] - v[c]) * fminf(e->wind[3] * dt, 1.0f);
            if (e->flags & PF_LIMIT) {
                float sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                float ns = fminf(fmaxf(sp, e->limits[1]), e->limits[0] > 0.0f ? e->limits[0] : sp);
                if (sp > 1e-4f)
                    for (int c = 0; c < 3; c++) v[c] *= ns / sp;
            }
            for (int c = 0; c < 3; c++) p[c] += v[c] * dt;
            if (e->flags & PF_VORTEX) {
                float ang = e->vortex[0] * 0.017453293f * dt, cs = cosf(ang), sn = sinf(ang);
                float x = p[0] * cs - p[1] * sn, y = p[0] * sn + p[1] * cs;
                p[0] = x, p[1] = y;
            }
            if ((e->flags & PF_COLLIDE) && p[2] < e->collide[0]) {
                if (e->collide[3] > 0.5f) dead = true;
                p[2] = e->collide[0], v[2] = -v[2] * e->collide[1];
                v[0] *= 1.0f - e->collide[2], v[1] *= 1.0f - e->collide[2];
            }
            if (e->flags & PF_KILLBOX) {
                bool in = p[0] > e->killbox_min[0] && p[1] > e->killbox_min[1] && p[2] > e->killbox_min[2] && p[0] < e->killbox_max[0] &&
                          p[1] < e->killbox_max[1] && p[2] < e->killbox_max[2];
                if (in == (e->killbox_min[3] > 0.5f)) dead = true;
            }
        }
        if (dead) continue;
        PrevPart* q = &out[n++];
        memcpy(q->p, p, sizeof p);
        float a = age / fmaxf(life, 1e-4f);
        q->size = size_of(e, r, a, v);
        color_of(e, r, a, q->col);
        if (e->flags & PF_SPEED_SCALE) {
            float sp = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            float u = fminf(fmaxf((sp - e->speed_scale[0]) / fmaxf(e->speed_scale[1] - e->speed_scale[0], 1e-3f), 0.0f), 1.0f);
            q->col[3] *= e->extra[1] + (e->extra[2] - e->extra[1]) * u;
        }
        if (e->flags & PF_MESH)
            for (int c = 0; c < 3; c++) q->col[c] *= e->mesh_color[c];
    }
    return n;
}

static uint32_t to_u32(float r, float g, float b, float a) {
    int R = (int)(fminf(fmaxf(r, 0.0f), 1.0f) * 255.0f + 0.5f), G = (int)(fminf(fmaxf(g, 0.0f), 1.0f) * 255.0f + 0.5f);
    int B = (int)(fminf(fmaxf(b, 0.0f), 1.0f) * 255.0f + 0.5f), A = (int)(fminf(fmaxf(a, 0.0f), 1.0f) * 255.0f + 0.5f);
    return (uint32_t)R | ((uint32_t)G << 8) | ((uint32_t)B << 16) | ((uint32_t)A << 24);
}

// The camera of the preview: orbiting the emitter's point at (0, 0, focus_z)
typedef struct PrevCam {
    float pos[3], right[3], up[3], fwd[3], cx, cy, f;
} PrevCam;

static bool project(const PrevCam* c, const float w[3], float* sx, float* sy, float* depth) {
    float rel[3] = { w[0] - c->pos[0], w[1] - c->pos[1], w[2] - c->pos[2] };
    float d = rel[0] * c->fwd[0] + rel[1] * c->fwd[1] + rel[2] * c->fwd[2];
    if (d < 5.0f) return false;
    *sx = c->cx + c->f * (rel[0] * c->right[0] + rel[1] * c->right[1] + rel[2] * c->right[2]) / d;
    *sy = c->cy - c->f * (rel[0] * c->up[0] + rel[1] * c->up[1] + rel[2] * c->up[2]) / d;
    *depth = d;
    return true;
}

static float system_end(const ParticleSystemAsset* a) {
    float end = 0.0f;
    for (uint32_t i = 0; i < a->nemitters; i++) {
        if (!a->e[i].on) continue;
        GpuEmitter g;
        ps_compile_cpu(&a->e[i], &g);
        float t = ps_end_time(&g);
        if (t <= 0.0f) return 0.0f; // (one plays forever)
        end = fmaxf(end, t);
    }
    return end;
}

static void preview_panel(ImVec2 size) {
    ParticleSystemAsset* a = E.a;
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 mn = igGetCursorScreenPos(), mx = { mn.x + size.x, mn.y + size.y };
    // (Q7) Niagara's preview scene: the asset viewport (its floor and grid, the studio or the sky, the camera), the
    // particles over it (a CPU sample of the simulation, the exact look is Preview in Level)
    static const float bmn[3] = { -150.0f, -150.0f, 0.0f }, bmx[3] = { 150.0f, 150.0f, 300.0f };
    AvpScene sc;
    memset(&sc, 0, sizeof sc);
    sc.sel_bone = sc.sel_body = sc.sel_constraint = -1, sc.mn = bmn, sc.mx = bmx;
    sc.hint = TR("Left drag: orbit, middle: pan, wheel: zoom, right drag + WASD: fly, F: frame");
    avp_draw(&E.vp, BP_PV_KEY_PARTICLES, mn.x, mn.y, size.x, size.y, &sc, NULL);
    ImDrawList_PushClipRect(dl, mn, mx, true);
    PrevCam c;
    memcpy(c.pos, E.vp.eye, 12), memcpy(c.right, E.vp.right, 12), memcpy(c.up, E.vp.up, 12), memcpy(c.fwd, E.vp.fwd, 12);
    c.cx = (mn.x + mx.x) * 0.5f, c.cy = (mn.y + mx.y) * 0.5f, c.f = E.vp.focal;
    // the emitters' particles (the additive ones brighter, drawn last)
    static PrevPart parts[PREVIEW_MAX];
    int total = 0;
    for (int pass = 0; pass < 2; pass++)
        for (uint32_t ei = 0; ei < a->nemitters; ei++) {
            if (!a->e[ei].on) continue;
            GpuEmitter g;
            ps_compile_cpu(&a->e[ei], &g);
            bool add = (g.flags & PF_ADDITIVE) != 0u;
            if (add != (pass == 1)) continue;
            int n = simulate(&g, E.t - g.extra[3], parts, PREVIEW_MAX);
            total += n;
            float glow = g.flags & PF_MESH ? 0.0f : g.render_p[3];
            float boost = add ? fminf(1.0f + glow / 4000.0f, 3.0f) : (g.flags & PF_LIT ? 0.85f : 1.0f);
            for (int i = 0; i < n; i++) {
                float sx, sy, d;
                if (!project(&c, parts[i].p, &sx, &sy, &d)) continue;
                float rad = fmaxf(parts[i].size * 0.5f * c.f / d, 0.8f);
                if (rad > size.y * 0.25f) rad = size.y * 0.25f;
                const float* q = parts[i].col;
                float al = q[3] * g.render2[1] * (add ? 0.75f : 1.0f);
                uint32_t col = to_u32(q[0] * boost, q[1] * boost, q[2] * boost, al);
                if (g.flags & PF_MESH) ImDrawList_AddRectFilled(dl, (ImVec2){ sx - rad, sy - rad }, (ImVec2){ sx + rad, sy + rad }, col, 0, 0);
                else if (rad < 1.5f) ImDrawList_AddRectFilled(dl, (ImVec2){ sx - rad, sy - rad }, (ImVec2){ sx + rad, sy + rad }, col, 0, 0);
                else {
                    if (!(g.render_p[0] >= 0.5f && g.render_p[0] < 1.5f) && rad > 3.0f) // (a soft sprite: a fainter halo around)
                        ImDrawList_AddCircleFilled(dl, (ImVec2){ sx, sy }, rad, to_u32(q[0] * boost, q[1] * boost, q[2] * boost, al * 0.35f), 12);
                    ImDrawList_AddCircleFilled(dl, (ImVec2){ sx, sy }, rad * 0.6f, col, rad > 6.0f ? 12 : 6);
                }
            }
        }
    ImDrawList_PopClipRect(dl);
    E.shown = total;
    char info[96];
    snprintf(info, sizeof info, TR("%.2f s, %d particles shown"), E.t, total);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ mn.x + 8, mn.y + igGetFrameHeight() + 14 }, THA(TK_N15, 176), info, NULL);
}

// ---------------------------------------------------------------------------------------------------------------------
// The parameters (the Selection panel): vectors (X Y Z), colors (R G B [A]), choices ("(0 a, 1 b...)"), toggles

static bool ends_with(const char* s, const char* end) {
    size_t n = strlen(s), m = strlen(end);
    return n >= m && !strcmp(s + n - m, end);
}

// "Name (0 a, 1 b, 2 c)": the name before " (" and the choices; false when it isn't one
static bool parse_choices(const char* param, char* name, size_t cap, char opts[8][40], int* nopts) {
    const char* par = strstr(param, " (0 ");
    if (!par) return false;
    size_t nl = (size_t)(par - param);
    if (nl >= cap) nl = cap - 1;
    memcpy(name, param, nl), name[nl] = 0;
    const char* p = par + 2;
    *nopts = 0;
    while (*p && *p != ')' && *nopts < 8) {
        while (*p == ' ' || *p == ',') p++;
        while (*p >= '0' && *p <= '9') p++; // (the number)
        while (*p == ' ') p++;
        int k = 0;
        while (*p && *p != ',' && *p != ')' && k < 39) opts[*nopts][k++] = *p++;
        opts[*nopts][k] = 0;
        if (k) (*nopts)++;
    }
    return *nopts > 1;
}

static bool is_toggle(const PsModuleInfo* mi, int k) {
    const char* n = mi->params[k];
    return mi->hi[k] == 1.0f && mi->lo[k] == 0.0f &&
           (strstr(n, "Lit") || strstr(n, "Additive") || strstr(n, "Aligned") || strstr(n, "Only") || strstr(n, "Die") || strstr(n, "Inside") ||
            strstr(n, "Shadows") || strstr(n, "Blend"));
}

// k starts a color: "R..", "G..", "B.." with the same suffix (and "A.."/"Alpha.." after: 4)
static int color_run(const PsModuleInfo* mi, int k) {
    if (k + 2 >= mi->nparams) return 0;
    const char *r = mi->params[k], *g = mi->params[k + 1], *b = mi->params[k + 2];
    if (r[0] != 'R' || g[0] != 'G' || b[0] != 'B' || strcmp(r + 1, g + 1) || strcmp(r + 1, b + 1)) return 0;
    if (r[1] && r[1] != ' ') return 0;
    if (k + 3 < mi->nparams) {
        const char* al = mi->params[k + 3];
        if ((al[0] == 'A' && !strcmp(al + 1, r + 1)) || (!strncmp(al, "Alpha", 5) && !strcmp(al + 5, r + 1))) return 4;
    }
    return 3;
}

// k starts a vector: "..X", "..Y", "..Z" with the same prefix
static int vector_run(const PsModuleInfo* mi, int k) {
    if (k + 2 >= mi->nparams) return 0;
    const char *x = mi->params[k], *y = mi->params[k + 1], *z = mi->params[k + 2];
    size_t n = strlen(x);
    if (!n || x[n - 1] != 'X' || strlen(y) != n || strlen(z) != n || y[n - 1] != 'Y' || z[n - 1] != 'Z') return 0;
    if (strncmp(x, y, n - 1) || strncmp(x, z, n - 1)) return 0;
    if (n > 1 && x[n - 2] != ' ') return 0;
    return 3;
}

static float drag_speed(const PsModuleInfo* mi, int k, float v) {
    float span = mi->hi[k] - mi->lo[k];
    return span > 1000.0f ? fmaxf(fabsf(v) * 0.01f, 0.5f) : fmaxf(span / 500.0f, 0.001f);
}

// The gradient of Color Over Life (4 stops at 0, Time 1, Time 2, 1)
static void draw_gradient(ImDrawList* dl, const PsModule* m, ImVec2 mn, ImVec2 mx) {
    float t[4] = { 0.0f, m->p[16], fmaxf(m->p[17], m->p[16]), 1.0f };
    // (a checker under it: the alpha shows)
    float cw = (mx.y - mn.y) * 0.5f;
    for (float x = mn.x; x < mx.x; x += cw)
        for (int r = 0; r < 2; r++)
            ImDrawList_AddRectFilled(dl, (ImVec2){ x, mn.y + r * cw }, (ImVec2){ fminf(x + cw, mx.x), mn.y + (r + 1) * cw },
                                     (((int)((x - mn.x) / cw) + r) & 1) ? TH(TK_N11) : TH(TK_N13), 0, 0);
    for (int s = 0; s < 3; s++) {
        const float *c0 = m->p + s * 4, *c1 = m->p + (s + 1) * 4;
        float x0 = mn.x + (mx.x - mn.x) * t[s], x1 = mn.x + (mx.x - mn.x) * t[s + 1];
        if (x1 <= x0) continue;
        uint32_t a = to_u32(c0[0], c0[1], c0[2], c0[3]), b = to_u32(c1[0], c1[1], c1[2], c1[3]);
        ImDrawList_AddRectFilledMultiColor(dl, (ImVec2){ x0, mn.y }, (ImVec2){ x1, mx.y }, a, b, b, a);
    }
    ImDrawList_AddRect(dl, mn, mx, TH(TK_N0), 0, 1.0f, 0);
}

static bool param_ui(PsModule* m) {
    const PsModuleInfo* mi = ps_module_info(m->kind);
    bool ch = false;
    float fs = igGetFontSize();
    if (m->kind == PM_COLOR_LIFE) { // the gradient over the stops
        ImVec2 p = igGetCursorScreenPos();
        float w = igGetContentRegionAvail().x;
        draw_gradient(igGetWindowDrawList(), m, p, (ImVec2){ p.x + w, p.y + fs * 1.2f });
        igDummy((ImVec2){ w, fs * 1.2f + 4 });
    }
    for (int k = 0; k < mi->nparams;) {
        igPushID_Int(k);
        char name[64], opts[8][40];
        int nopts = 0, run;
        igSetNextItemWidth(-fs * 8.0f);
        if ((run = color_run(mi, k))) {
            char label[64];
            const char* suf = mi->params[k] + 1;
            snprintf(label, sizeof label, "%s%s", TR("Color"), suf);
            ImGuiColorEditFlags fl = ImGuiColorEditFlags_Float | ImGuiColorEditFlags_HDR | (run == 3 ? ImGuiColorEditFlags_NoAlpha : 0) |
                                     ImGuiColorEditFlags_AlphaPreviewHalf;
            if (run == 4) ch |= igColorEdit4(label, &m->p[k], fl);
            else ch |= igColorEdit3(label, &m->p[k], fl);
        } else if ((run = vector_run(mi, k))) {
            char label[64];
            size_t n = strlen(mi->params[k]);
            if (n > 2) snprintf(label, sizeof label, "%.*s", (int)(n - 2), mi->params[k]);
            else snprintf(label, sizeof label, "%s", TR(mi->name));
            ch |= igDragFloat3(label, &m->p[k], drag_speed(mi, k, m->p[k]), mi->lo[k], mi->hi[k], "%.4g", ImGuiSliderFlags_AlwaysClamp);
        } else if (parse_choices(mi->params[k], name, sizeof name, opts, &nopts)) {
            run = 1;
            int cur = (int)(m->p[k] + 0.5f);
            if (cur < 0) cur = 0;
            if (cur >= nopts) cur = nopts - 1;
            if (igBeginCombo(name, opts[cur], 0)) {
                for (int o = 0; o < nopts; o++)
                    if (igSelectable_Bool(opts[o], o == cur, 0, (ImVec2){ 0, 0 })) m->p[k] = (float)o, ch = true;
                igEndCombo();
            }
        } else if (is_toggle(mi, k)) {
            run = 1;
            bool b = m->p[k] > 0.5f;
            if (igCheckbox(TR(mi->params[k]), &b)) m->p[k] = b ? 1.0f : 0.0f, ch = true;
        } else {
            run = 1;
            ch |= igDragFloat(TR(mi->params[k]), &m->p[k], drag_speed(mi, k, m->p[k]), mi->lo[k], mi->hi[k], "%.4g", ImGuiSliderFlags_AlwaysClamp);
        }
        for (int j = k; j < k + run; j++) // (kept in range: a color's HDR values too)
            m->p[j] = fminf(fmaxf(m->p[j], mi->lo[j]), mi->hi[j]);
        igPopID();
        k += run;
    }
    if (mi->asset == 2) { // the mesh
        uint32_t cur = render_mesh_slot(m->asset);
        igSetNextItemWidth(-fs * 8.0f);
        if (igBeginCombo(TR("Mesh"), cur < SHAPE_COUNT ? TR(g_shape_names[cur]) : render_mesh_name(cur), 0)) {
            for (uint32_t s = 0; s < render_mesh_count(); s++)
                if (igSelectable_Bool(s < SHAPE_COUNT ? TR(g_shape_names[s]) : render_mesh_name(s), cur == s, 0, (ImVec2){ 0, 0 }))
                    m->asset = render_mesh_id(s), ch = true;
            igEndCombo();
        }
    } else if (mi->asset == 1) { // a material's texture (shape 3)
        Material* cur = m->asset ? mat_find_hash(m->asset) : NULL;
        igSetNextItemWidth(-fs * 8.0f);
        if (igBeginCombo(TR("Texture (Material)"), cur ? cur->name : TR("(none)"), 0)) {
            if (igSelectable_Bool(TR("(none)"), !cur, 0, (ImVec2){ 0, 0 })) m->asset = 0, ch = true;
            for (uint32_t i = 0; i < mat_count(); i++) {
                Material* mt = mat_at(i);
                if (mt && igSelectable_Bool(mt->name, mt == cur, 0, (ImVec2){ 0, 0 })) m->asset = mt->hash, ch = true;
            }
            igEndCombo();
        }
    }
    return ch;
}

// ---------------------------------------------------------------------------------------------------------------------
// Adding modules (the "+" of a group): its last 2 used and 2 recommended, then all of the group

typedef struct ModAccept { int stage; const PsEmitter* e; const char* shown[2]; } ModAccept;
static bool mod_accept(const char* item, void* user) {
    const ModAccept* u = user;
    int k = ps_module_kind(item);
    if (k < 0 || ps_module_info(k)->stage != u->stage) return false;
    for (uint32_t i = 0; i < u->e->nmodules; i++)
        if (u->e->m[i].kind == k) return false;
    return (!u->shown[0] || strcmp(u->shown[0], item)) && (!u->shown[1] || strcmp(u->shown[1], item));
}

static bool add_module_menu(PsEmitter* e, int stage) {
    bool ch = false;
    const char* ctx = E.module >= 0 && E.module < (int)e->nmodules ? ps_module_info(e->m[E.module].kind)->name : "";
    ModAccept u = { stage, e, { NULL, NULL } };
    const char* items[8];
    int pick = -1, n = gm_recent(GM_PARTICLES, items, 8), ns = 0;
    for (int i = 0; i < n && ns < GM_RECENT_SHOWN; i++) {
        if (!mod_accept(items[i], &u)) continue;
        if (!ns) igSeparatorText(gm_title_recent());
        igPushID_Str("pr"), igPushID_Int(ns);
        if (ui_menu_item(ICON_PARTICLES, TR(items[i]), NULL, false, true)) pick = ps_module_kind(items[i]);
        igPopID(), igPopID();
        u.shown[ns++] = items[i];
    }
    n = gm_recommend(GM_PARTICLES, ctx, mod_accept, &u, items, GM_RECOMMEND_SHOWN);
    for (int i = 0; i < n; i++) {
        if (!i) igSeparatorText(gm_title_recommended());
        igPushID_Str("pm"), igPushID_Int(i);
        if (ui_menu_item(ICON_PARTICLES, TR(items[i]), NULL, false, true)) pick = ps_module_kind(items[i]);
        igPopID(), igPopID();
    }
    igSeparatorText(TR(g_stage_names[stage]));
    for (int k = 0; k < PM_COUNT; k++) {
        const PsModuleInfo* mi = ps_module_info(k);
        if (mi->stage != stage) continue;
        bool have = false;
        for (uint32_t i = 0; i < e->nmodules; i++) have |= e->m[i].kind == k;
        if (ui_menu_item(ICON_PARTICLES, TR(mi->name), NULL, false, !have)) pick = k;
        if (igIsItemHovered(0)) igSetTooltip("%s", TR(mi->tip));
    }
    if (pick >= 0) {
        gm_note(GM_PARTICLES, ctx, ps_module_info(pick)->name);
        PsModule* m = ps_add_module(e, pick);
        if (m) E.module = (int)(m - e->m), E.open_module = E.module, ch = true;
    }
    return ch;
}

// ---------------------------------------------------------------------------------------------------------------------
// The System Overview: the System node, then a node per emitter with its colored groups

static void group_bar(uint32_t col, const char* text, float w) {
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p = igGetCursorScreenPos();
    float h = igGetFontSize() + 6.0f;
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + h }, (col & 0x00FFFFFFu) | 0x50000000u, 3.0f, 0);
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + 4.0f, p.y + h }, col, 2.0f, 0);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + 10.0f, p.y + 3.0f }, TH(TK_NODE_TITLE), text, NULL);
    igDummy((ImVec2){ w, h });
}

// A node's title bar at the cursor; the cursor is left inside it (for its checkbox and name). Returns its top left
static ImVec2 node_header(uint32_t col, float w, bool selected) {
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p = igGetCursorScreenPos();
    float h = igGetFontSize() * 1.9f;
    if (graph_orbit()) { // (Q5) the emitter's header as a card's band (8 px, the Effects family's icon, the halo when selected)
        graph_card(dl, p, (ImVec2){ p.x + w, p.y + h }, h, col, ICON_PARTICLES, selected, false, false, 1.0f);
        igSetCursorScreenPos((ImVec2){ graph_card_title_x(p, h, ICON_PARTICLES, 1.0f), p.y + (h - igGetFrameHeight()) * 0.5f });
        return p;
    }
    ImDrawList_AddRectFilled(dl, p, (ImVec2){ p.x + w, p.y + h }, (col & 0x00FFFFFFu) | 0xC0000000u, 4.0f, ImDrawFlags_RoundCornersTop);
    if (selected) ImDrawList_AddRect(dl, p, (ImVec2){ p.x + w, p.y + h }, TH(TK_N15), 4.0f, 2.0f, ImDrawFlags_RoundCornersTop);
    igSetCursorScreenPos((ImVec2){ p.x + 6, p.y + (h - igGetFrameHeight()) * 0.5f });
    return p;
}
// Below the title bar
static void node_body(ImVec2 top) { igSetCursorScreenPos((ImVec2){ top.x, top.y + igGetFontSize() * 1.9f + 4 }); }

static bool system_node(ParticleSystemAsset* a, float w) {
    bool ch = false;
    float fs = igGetFontSize();
    igBeginGroup();
    ImVec2 top = node_header(k_system_color, w, E.emitter == SEL_SYSTEM);
    if (igSelectable_Bool(a->name, E.emitter == SEL_SYSTEM, 0, (ImVec2){ w - 16, 0 })) E.emitter = SEL_SYSTEM, E.module = -1;
    node_body(top);
    group_bar(k_system_color, TR("Emitters"), w);
    for (uint32_t i = 0; i < a->nemitters; i++) {
        igPushID_Int((int)i);
        bool on = a->e[i].on != 0;
        if (igCheckbox("##on", &on)) a->e[i].on = on, ch = true;
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("Plays this emitter (off: it stays in the asset, silent)."));
        igSameLine(0, 6);
        if (igSelectable_Bool(a->e[i].name, E.emitter == (int)i, 0, (ImVec2){ w - fs * 2.2f, 0 })) E.emitter = (int)i, E.module = -1;
        igPopID();
    }
    if (a->nemitters < PS_MAX_EMITTERS && igButton(TR("+ Emitter"), (ImVec2){ w, 0 })) igOpenPopup_Str("add_em", 0);
    if (igBeginPopup("add_em", 0)) {
        igTextDisabled("%s", TR("A template's emitter"));
        for (int t = 0; t < PST_COUNT; t++)
            if (ui_menu_item(ICON_NEW, TR(g_ps_template_names[t]), NULL, false, true)) {
                ParticleSystemAsset tmp;
                memset(&tmp, 0, sizeof tmp);
                ps_template(&tmp, t);
                a->e[a->nemitters] = tmp.e[0];
                a->e[a->nemitters].seed = 7u + a->nemitters * 104729u;
                E.emitter = (int)a->nemitters++, E.module = -1, ch = true;
            }
        igEndPopup();
    }
    igEndGroup();
    return ch;
}

static bool emitter_node(ParticleSystemAsset* a, int ei, float w) {
    PsEmitter* e = &a->e[ei];
    bool ch = false, sel = E.emitter == ei;
    float fs = igGetFontSize();
    igPushID_Int(ei + 100);
    igBeginGroup();
    ImVec2 top = node_header(e->on ? TH(g_stage_colors[0]) : TH(TK_N10), w, sel);
    bool on = e->on != 0;
    if (igCheckbox("##eon", &on)) e->on = on, ch = true;
    igSameLine(0, 6);
    if (igSelectable_Bool(e->name, sel && E.module < 0, 0, (ImVec2){ w - fs * 2.6f, 0 })) E.emitter = ei, E.module = -1;
    if (igBeginPopupContextItem("em_ctx", ImGuiPopupFlags_MouseButtonRight)) {
        if (ui_menu_item(ICON_DUPLICATE, TR("Duplicate Emitter"), NULL, false, a->nemitters < PS_MAX_EMITTERS)) {
            a->e[a->nemitters] = *e;
            a->e[a->nemitters].seed = e->seed * 31u + 17u;
            snprintf(a->e[a->nemitters].name, PS_NAME, "%.40s 2", e->name);
            E.emitter = (int)a->nemitters++, E.module = -1, ch = true;
        }
        if (ui_menu_item(ICON_DELETE, TR("Remove Emitter"), NULL, false, a->nemitters > 1)) {
            memmove(&a->e[ei], &a->e[ei + 1], sizeof(PsEmitter) * (a->nemitters - (uint32_t)ei - 1));
            a->nemitters--, E.emitter = 0, E.module = -1, ch = true;
            igEndPopup();
            igEndGroup();
            igPopID();
            return true;
        }
        igEndPopup();
    }
    node_body(top);
    for (int st = 0; st < PSS_COUNT; st++) {
        igPushID_Int(st);
        group_bar(TH(g_stage_colors[st]), TR(g_stage_names[st]), w - fs * 1.6f);
        igSameLine(0, 2);
        if (ui_icon_button("##add", ICON_PLUS, NULL, TH(TK_N15), false, TR("Add module"))) E.emitter = ei, igOpenPopup_Str("add_mod", 0);
        if (igBeginPopup("add_mod", 0)) {
            ch |= add_module_menu(e, st);
            igEndPopup();
        }
        for (uint32_t i = 0; i < e->nmodules; i++) {
            PsModule* m = &e->m[i];
            if (ps_module_info(m->kind)->stage != st) continue;
            igPushID_Int((int)i);
            bool mon = m->on != 0;
            igSetCursorPosX(igGetCursorPosX() + 6);
            if (igCheckbox("##on", &mon)) m->on = mon, ch = true;
            igSameLine(0, 6);
            float sw = w - fs * 3.6f; // (the name's row: positive, the remove button after it)
            bool msel = sel && E.module == (int)i;
            if (!m->on) igPushStyleColor_U32(ImGuiCol_Text, TH(TK_N11));
            if (igSelectable_Bool(TR(ps_module_info(m->kind)->name), msel, 0, (ImVec2){ sw, 0 }))
                E.emitter = ei, E.module = (int)i, E.open_module = (int)i;
            if (!m->on) igPopStyleColor(1);
            if (igIsItemHovered(ImGuiHoveredFlags_DelayNormal)) igSetTooltip("%s", TR(ps_module_info(m->kind)->tip));
            igSameLine(0, 2);
            if (ui_icon_button("##rm", ICON_DELETE, NULL, TH(TK_N13), false, TR("Remove module"))) {
                ps_remove_module(e, i);
                if (E.emitter == ei && E.module >= (int)e->nmodules) E.module = -1;
                ch = true;
                igPopID();
                break;
            }
            igPopID();
        }
        igPopID();
    }
    igEndGroup();
    // the node's frame
    ImVec2 mn = igGetItemRectMin(), mx = igGetItemRectMax();
    ImDrawList_AddRect(igGetWindowDrawList(), (ImVec2){ mn.x - 3, mn.y }, (ImVec2){ mx.x + 3, mx.y + 3 }, sel ? TH(TK_N14) : TH(TK_N8), 5.0f,
                       sel ? 2.0f : 1.0f, 0);
    igPopID();
    return ch;
}

static bool overview_panel(ParticleSystemAsset* a) {
    bool ch = false;
    float fs = igGetFontSize(), w = fs * 15.0f;
    // (a dotted canvas, like Niagara's graph)
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 o = igGetWindowPos(), sz = igGetWindowSize();
    for (float y = o.y; y < o.y + sz.y; y += 24.0f)
        for (float x = o.x; x < o.x + sz.x; x += 24.0f) ImDrawList_AddRectFilled(dl, (ImVec2){ x, y }, (ImVec2){ x + 1.5f, y + 1.5f }, THA(TK_N15, 48), 0, 0);
    igSetCursorPos((ImVec2){ igGetCursorPosX() + 6, igGetCursorPosY() + 6 });
    ch |= system_node(a, w * 0.9f);
    for (uint32_t i = 0; i < a->nemitters; i++) {
        igSameLine(0, fs * 1.6f);
        if (emitter_node(a, (int)i, w)) {
            ch = true;
            if (i >= a->nemitters) break;
        }
    }
    return ch;
}

// ---------------------------------------------------------------------------------------------------------------------
// Selection: the System's or an emitter's properties and its whole stack, unfolded

static bool emitter_props(PsEmitter* e) {
    bool ch = false;
    float fs = igGetFontSize();
    igSetNextItemWidth(-fs * 8.0f);
    ch |= igInputText(TR("Name"), e->name, sizeof e->name, 0, NULL, NULL);
    bool local = e->local != 0;
    if (igCheckbox(TR("Local Space"), &local)) e->local = local, ch = true;
    if (igIsItemHovered(0)) igSetTooltip("%s", TR("The particles move with the actor (else they stay where they were born)."));
    int seed = (int)e->seed, mx = (int)e->max_particles;
    igSetNextItemWidth(-fs * 8.0f);
    if (igInputInt(TR("Seed"), &seed, 1, 10, 0)) e->seed = (uint32_t)seed, ch = true;
    igSetNextItemWidth(-fs * 8.0f);
    if (igInputInt(TR("Max Particles (0: auto)"), &mx, 1000, 100000, 0)) e->max_particles = (uint32_t)(mx < 0 ? 0 : mx), ch = true;
    return ch;
}

static bool selection_panel(ParticleSystemAsset* a) {
    bool ch = false;
    if (E.emitter == SEL_SYSTEM) {
        igSeparatorText(TR("System"));
        igText("%s", a->name);
        igTextDisabled("%s", a->path);
        float end = system_end(a);
        if (end > 0.0f) igText(TR("Plays %.2f s (then the editor starts it again)"), end);
        else igText("%s", TR("Plays until stopped (an emitter loops forever)"));
        const ParticleStats* st = particles_stats();
        igText(TR("In the level: %u emitters playing, %u slots, %u sprites + %u meshes drawn, %.3f ms CPU"), st->emitters, st->capacity, st->sprites,
               st->meshes, st->cpu_ms);
        igSeparatorText(TR("Emitters"));
        for (uint32_t i = 0; i < a->nemitters; i++) {
            GpuEmitter g;
            ps_compile_cpu(&a->e[i], &g);
            igBulletText(TR("%s: up to %u particles%s"), a->e[i].name, g.capacity, a->e[i].on ? "" : TR(" (off)"));
        }
        return false;
    }
    if (E.emitter < 0 || E.emitter >= (int)a->nemitters) return false;
    PsEmitter* e = &a->e[E.emitter];
    // the emitter's own properties (Niagara's Emitter Properties)
    igPushStyleColor_U32(ImGuiCol_Header, THA(g_stage_colors[0], 0x90));
    if (igCollapsingHeader_TreeNodeFlags(TR("Emitter Properties"), E.module < 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
        igIndent(6);
        ch |= emitter_props(e);
        igUnindent(6);
    }
    igPopStyleColor(1);
    for (int st = 0; st < PSS_COUNT; st++) {
        igPushID_Int(st);
        group_bar(TH(g_stage_colors[st]), TR(g_stage_names[st]), igGetContentRegionAvail().x);
        for (uint32_t i = 0; i < e->nmodules; i++) {
            PsModule* m = &e->m[i];
            if (ps_module_info(m->kind)->stage != st) continue;
            igPushID_Int((int)i);
            if (E.open_module == (int)i) igSetNextItemOpen(true, ImGuiCond_Always);
            igPushStyleColor_U32(ImGuiCol_Header, E.module == (int)i ? TH(TK_BLUE_D) : TH(TK_N8));
            char label[96];
            snprintf(label, sizeof label, "%s###mod", TR(ps_module_info(m->kind)->name));
            bool open = igTreeNodeEx_Str(label, ImGuiTreeNodeFlags_Framed | ImGuiTreeNodeFlags_AllowOverlap | ImGuiTreeNodeFlags_SpanAvailWidth);
            igPopStyleColor(1);
            if (igIsItemClicked(0)) E.module = (int)i;
            if (E.open_module == (int)i) igSetScrollHereY(0.1f), E.open_module = -1;
            igSameLine(igGetCursorPosX() + igGetContentRegionAvail().x - igGetFrameHeight(), 0);
            bool mon = m->on != 0;
            if (igCheckbox("##on", &mon)) m->on = mon, ch = true;
            if (open) {
                igTextWrapped("%s", TR(ps_module_info(m->kind)->tip));
                ch |= param_ui(m);
                igTreePop();
            }
            igPopID();
        }
        igPopID();
    }
    return ch;
}

// ---------------------------------------------------------------------------------------------------------------------
// The bottom: Curves (the selected emitter's Color / Size Over Life) and the Timeline

static bool curves_panel(PsEmitter* e) {
    bool ch = false;
    float fs = igGetFontSize(), w = igGetContentRegionAvail().x, h = igGetContentRegionAvail().y - 4;
    PsModule* col = NULL;
    PsModule* sz = NULL;
    for (uint32_t i = 0; i < e->nmodules; i++) {
        if (e->m[i].kind == PM_COLOR_LIFE) col = &e->m[i];
        if (e->m[i].kind == PM_SIZE_LIFE) sz = &e->m[i];
    }
    if (!col && !sz) {
        igTextDisabled("%s", TR("This emitter has no curves: add Color Over Life or Size Over Life (Particle Update)."));
        if (igButton(TR("Add Color Over Life"), (ImVec2){ 0, 0 }) && ps_add_module(e, PM_COLOR_LIFE)) ch = true;
        igSameLine(0, 6);
        if (igButton(TR("Add Size Over Life"), (ImVec2){ 0, 0 }) && ps_add_module(e, PM_SIZE_LIFE)) ch = true;
        return ch;
    }
    ImDrawList* dl = igGetWindowDrawList();
    float half = col && sz ? (w - fs) * 0.5f : w;
    if (col) { // the gradient; the two middle stops dragged along it
        ImVec2 p = igGetCursorScreenPos();
        igText("%s", TR("Color Over Life"));
        ImVec2 g0 = { p.x, p.y + fs * 1.4f }, g1 = { p.x + half, p.y + fs * 1.4f + fmaxf(h - fs * 3.0f, fs * 2.0f) };
        draw_gradient(dl, col, g0, g1);
        for (int s = 0; s < 2; s++) {
            float x = g0.x + (g1.x - g0.x) * col->p[16 + s];
            ImDrawList_AddTriangleFilled(dl, (ImVec2){ x, g1.y + 1 }, (ImVec2){ x - 6, g1.y + 11 }, (ImVec2){ x + 6, g1.y + 11 }, TH(TK_N15));
            igSetCursorScreenPos((ImVec2){ x - 7, g1.y });
            igPushID_Int(s);
            igInvisibleButton("##stop", (ImVec2){ 14, 12 }, 0);
            if (igIsItemActive()) {
                float t = (igGetIO_Nil()->MousePos.x - g0.x) / fmaxf(g1.x - g0.x, 1.0f);
                col->p[16 + s] = fminf(fmaxf(t, s ? col->p[16] : 0.0f), s ? 1.0f : col->p[17]);
                ch = true;
            }
            if (igIsItemHovered(0)) igSetTooltip("%s", TR("Drag: the stop's time along the life"));
            igPopID();
        }
        igSetCursorScreenPos((ImVec2){ p.x, g1.y + 14 });
        igDummy((ImVec2){ half, 1 });
        if (sz) igSameLine(0, fs), igSetCursorScreenPos((ImVec2){ p.x + half + fs, p.y });
    }
    if (sz) { // the curve; its four points dragged up and down
        ImVec2 p = igGetCursorScreenPos();
        igText("%s", TR("Size Over Life"));
        ImVec2 c0 = { p.x, p.y + fs * 1.4f }, c1 = { p.x + half, p.y + fs * 1.4f + fmaxf(h - fs * 2.0f, fs * 2.0f) };
        ImDrawList_AddRectFilled(dl, c0, c1, TH(TK_N5), 3.0f, 0);
        float top = 0.0f;
        for (int k = 0; k < 4; k++) top = fmaxf(top, sz->p[k]);
        top = fmaxf(ceilf(top * 1.25f), 1.0f);
        for (int g = 1; g < 4; g++) {
            float y = c1.y - (c1.y - c0.y) * (float)g / 4.0f;
            ImDrawList_AddLine(dl, (ImVec2){ c0.x, y }, (ImVec2){ c1.x, y }, THA(TK_N15, 48), 1.0f);
        }
        ImVec2 pts[4];
        for (int k = 0; k < 4; k++)
            pts[k] = (ImVec2){ c0.x + (c1.x - c0.x) * (float)k / 3.0f, c1.y - (c1.y - c0.y) * fminf(sz->p[k] / top, 1.0f) };
        ImDrawList_AddPolyline(dl, pts, 4, TH(TK_AMBER), 2.0f, 0);
        for (int k = 0; k < 4; k++) {
            ImDrawList_AddCircleFilled(dl, pts[k], 5.0f, TH(TK_N15), 10);
            igSetCursorScreenPos((ImVec2){ pts[k].x - 7, pts[k].y - 7 });
            igPushID_Int(k + 10);
            igInvisibleButton("##pt", (ImVec2){ 14, 14 }, 0);
            if (igIsItemActive()) {
                float v = (c1.y - igGetIO_Nil()->MousePos.y) / fmaxf(c1.y - c0.y, 1.0f) * top;
                sz->p[k] = fminf(fmaxf(v, 0.0f), 100.0f), ch = true;
            }
            if (igIsItemHovered(0) || igIsItemActive()) igSetTooltip("%.2fx", sz->p[k]);
            igPopID();
        }
        char t[32];
        snprintf(t, sizeof t, "%.0fx", top);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ c0.x + 4, c0.y + 2 }, THA(TK_N15, 144), t, NULL);
        igSetCursorScreenPos((ImVec2){ p.x, c1.y + 4 });
        igDummy((ImVec2){ half, 1 });
    }
    return ch;
}

static void timeline_panel(ParticleSystemAsset* a) {
    float fs = igGetFontSize(), w = igGetContentRegionAvail().x, name_w = fs * 9.0f;
    float end = system_end(a), span = end > 0.0f ? end * 1.15f : fmaxf(E.t * 1.25f, 6.0f);
    span = fmaxf(span, 1.0f);
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p = igGetCursorScreenPos();
    float x0 = p.x + name_w, x1 = p.x + w - 8.0f, row = fs * 1.5f;
    // the ruler
    float tick = span > 30.0f ? 10.0f : span > 8.0f ? 2.0f : span > 3.0f ? 0.5f : 0.25f;
    for (float t = 0.0f; t <= span + 1e-3f; t += tick) {
        float x = x0 + (x1 - x0) * t / span;
        ImDrawList_AddLine(dl, (ImVec2){ x, p.y + fs }, (ImVec2){ x, p.y + fs * 1.4f }, THA(TK_N15, 128), 1.0f);
        char l[16];
        snprintf(l, sizeof l, "%g", t);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ x + 2, p.y }, THA(TK_N15, 144), l, NULL);
    }
    float y = p.y + fs * 1.6f;
    for (uint32_t i = 0; i < a->nemitters; i++, y += row) {
        GpuEmitter g;
        ps_compile_cpu(&a->e[i], &g);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ p.x + 4, y + 2 }, a->e[i].on ? TH(TK_N14) : TH(TK_N11), a->e[i].name, NULL);
        float d = g.extra[3];
        uint32_t c = a->e[i].on ? TH(g_stage_colors[1]) : TH(TK_N10);
        if (g.rate > 0.0f) { // the continuous spawning
            float s = g.stop > 1e30f ? span : fminf(g.stop + d, span);
            ImDrawList_AddRectFilled(dl, (ImVec2){ x0 + (x1 - x0) * d / span, y + 3 }, (ImVec2){ x0 + (x1 - x0) * s / span, y + row - 3 }, c, 3.0f, 0);
            float lend = g.stop > 1e30f ? span : fminf(g.stop + d + g.life_max, span); // (and the last particles' lives)
            ImDrawList_AddRectFilled(dl, (ImVec2){ x0 + (x1 - x0) * s / span, y + row * 0.4f }, (ImVec2){ x0 + (x1 - x0) * lend / span, y + row * 0.6f },
                                     (c & 0x00FFFFFFu) | 0x60000000u, 2.0f, 0);
        }
        if (g.burst_count > 0.0f) // the bursts: diamonds
            for (int k = 0; k < 200; k++) {
                float t = g.burst_time + (float)k * g.burst_interval + d;
                if (t > span || t > g.stop + d || (k && g.burst_interval <= 0.0f)) break;
                float x = x0 + (x1 - x0) * t / span, cy = y + row * 0.5f;
                ImDrawList_AddQuadFilled(dl, (ImVec2){ x, cy - 6 }, (ImVec2){ x + 6, cy }, (ImVec2){ x, cy + 6 }, (ImVec2){ x - 6, cy }, TH(TK_AMBER));
            }
    }
    // the playhead (dragged: scrubs the preview)
    float px = x0 + (x1 - x0) * fminf(E.t / span, 1.0f);
    ImDrawList_AddLine(dl, (ImVec2){ px, p.y }, (ImVec2){ px, y }, TH(TK_RED), 2.0f);
    igSetCursorScreenPos((ImVec2){ x0, p.y });
    igInvisibleButton("##scrub", (ImVec2){ fmaxf(x1 - x0, 1.0f), fmaxf(y - p.y, row) }, 0);
    if (igIsItemActive()) E.t = fminf(fmaxf((igGetIO_Nil()->MousePos.x - x0) / fmaxf(x1 - x0, 1.0f) * span, 0.0f), span), E.paused = true;
    if (igIsItemHovered(0)) igSetTooltip("%s", TR("Drag to scrub the preview (it pauses)"));
}

// ---------------------------------------------------------------------------------------------------------------------

static void play_pause_button(void) {
    ImVec2 p = igGetCursorScreenPos();
    float s = igGetFrameHeight();
    if (igButton("##play", (ImVec2){ s * 1.4f, s })) E.paused = !E.paused;
    if (igIsItemHovered(0)) igSetTooltip("%s", E.paused ? TR("Play the preview") : TR("Pause the preview"));
    ImDrawList* dl = igGetWindowDrawList();
    float cx = p.x + s * 0.7f, cy = p.y + s * 0.5f, r = s * 0.28f;
    if (E.paused)
        ImDrawList_AddTriangleFilled(dl, (ImVec2){ cx - r * 0.7f, cy - r }, (ImVec2){ cx + r, cy }, (ImVec2){ cx - r * 0.7f, cy + r }, TH(TK_GREEN));
    else {
        ImDrawList_AddRectFilled(dl, (ImVec2){ cx - r * 0.8f, cy - r }, (ImVec2){ cx - r * 0.2f, cy + r }, TH(TK_N14), 0, 0);
        ImDrawList_AddRectFilled(dl, (ImVec2){ cx + r * 0.2f, cy - r }, (ImVec2){ cx + r * 0.8f, cy + r }, TH(TK_N14), 0, 0);
    }
}

void particle_editor_draw(Editor* ed) {
    if (!E.a) return;
    ParticleSystemAsset* a = E.a;
    if (E.emitter >= (int)a->nemitters) E.emitter = a->nemitters ? 0 : -1;
    ImGuiIO* io = igGetIO_Nil();
    // the preview's clock: looped after the system ends (as the editor plays it in the level)
    if (!E.paused) {
        E.t += io->DeltaTime * E.speed;
        float end = system_end(a);
        if (end > 0.0f && E.t > end + 0.5f) E.t = 0.0f;
        if (end <= 0.0f && E.t > 600.0f) E.t = 0.0f;
    }
    char title[160];
    snprintf(title, sizeof title, "%s%s - %s###particle_editor", a->name, E.unsaved ? "*" : "", TR("Particle System"));
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 92, igGetFontSize() * 50 }, ImGuiCond_FirstUseEver);
    bool open = true, ch = false;
    if (igBegin(title, &open, 0)) {
        float fs = igGetFontSize();
        // the toolbar
        bool save = ui_icon_button("##save", ICON_SAVE, TR("Save"), TH(TK_N15), false, TR("Saves the Particle System (Ctrl+S)")) ||
                    (igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && io->KeyCtrl && igIsKeyPressed_Bool(ImGuiKey_S, false));
        igSameLine(0, 4);
        if (ui_icon_button("##browse", ICON_SEARCH, TR("Browse"), TH(TK_N15), false, TR("Shows the asset in the Content Browser")))
            content_browse_to(a->path[0] == '/' ? a->path : "/Content"), ed->content_docked = true;
        igSameLine(0, 14);
        play_pause_button();
        igSameLine(0, 4);
        if (ui_icon_button("##restart", ICON_UNDO, TR("Restart"), TH(TK_N15), false, TR("Plays the system again from the start"))) {
            E.t = 0.0f, E.paused = false;
            if (E.preview != ENTITY_NONE) particles_activate(&ed->world, E.preview, true, true);
        }
        igSameLine(0, 4);
        igSetNextItemWidth(fs * 5.0f);
        static const float speeds[] = { 0.1f, 0.25f, 0.5f, 1.0f, 2.0f };
        char sp[16];
        snprintf(sp, sizeof sp, "%gx", E.speed);
        if (igBeginCombo("##speed", sp, 0)) {
            for (int i = 0; i < 5; i++) {
                char l[16];
                snprintf(l, sizeof l, "%gx", speeds[i]);
                if (igSelectable_Bool(l, E.speed == speeds[i], 0, (ImVec2){ 0, 0 })) E.speed = speeds[i];
            }
            igEndCombo();
        }
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("The preview's speed"));
        igSameLine(0, 14);
        bool pv = E.preview != ENTITY_NONE && ecs_alive(&ed->world, E.preview);
        if (igCheckbox(TR("Preview in Level"), &pv)) preview(ed, pv);
        if (igIsItemHovered(0)) igSetTooltip("%s", TR("Plays the system in front of the camera; every change shows at once."));
        if (save) {
            bool ok = ps_save(a);
            snprintf(E.status, sizeof E.status, "%s", ok ? TR("Saved.") : TR("Could not save."));
            if (ok) E.unsaved = false;
        }
        if (E.status[0]) igSameLine(0, 10), igTextDisabled("%s", E.status);
        // the panels
        // (2026-11-10) the bars between the panels drag them, as in Unreal's Niagara (double-click: back)
        static float s_left, s_right, s_bottom;
        float avail_h = igGetContentRegionAvail().y, W = igGetContentRegionAvail().x;
        float bottom_def = fmaxf(fs * 10.0f, avail_h * 0.24f), left_def = fmaxf(W * 0.27f, fs * 16), right_def = fmaxf(W * 0.30f, fs * 18);
        if (s_left <= 0.0f) s_left = left_def;
        if (s_right <= 0.0f) s_right = right_def;
        if (s_bottom <= 0.0f) s_bottom = bottom_def;
        s_bottom = fminf(s_bottom, fmaxf(avail_h - 160.0f, 80.0f));
        float bottom_h = s_bottom, top_h = avail_h - bottom_h - UI_SPLIT;
        float left_w = fminf(s_left, fmaxf(W - s_right - 200.0f, 120.0f)), right_w = fminf(s_right, fmaxf(W - left_w - 200.0f, 140.0f));
        igBeginChild_Str("left", (ImVec2){ left_w, top_h }, ImGuiChildFlags_Borders, 0);
        igSeparatorText(TR("Preview"));
        float ph = igGetContentRegionAvail().y;
        preview_panel((ImVec2){ igGetContentRegionAvail().x, ph });
        igEndChild();
        igSameLine(0, 0);
        ui_splitter("##ps_lsplit", true, &s_left, 1.0f, 120.0f, W - right_w - 200.0f, top_h, left_def);
        igSameLine(0, 0);
        igBeginChild_Str("overview", (ImVec2){ W - left_w - right_w - 2.0f * UI_SPLIT, top_h }, ImGuiChildFlags_Borders, ImGuiWindowFlags_HorizontalScrollbar);
        igSeparatorText(TR("System Overview"));
        ch |= overview_panel(a);
        igEndChild();
        igSameLine(0, 0);
        ui_splitter("##ps_rsplit", true, &s_right, -1.0f, 140.0f, W - left_w - 200.0f, top_h, right_def);
        igSameLine(0, 0);
        igBeginChild_Str("selection", (ImVec2){ 0, top_h }, ImGuiChildFlags_Borders, 0);
        igSeparatorText(TR("Selection"));
        ch |= selection_panel(a);
        igEndChild();
        ui_splitter("##ps_bsplit", false, &s_bottom, -1.0f, 80.0f, avail_h - 160.0f, W, bottom_def);
        igBeginChild_Str("bottom", (ImVec2){ 0, 0 }, ImGuiChildFlags_Borders, 0);
        if (igBeginTabBar("##bt", 0)) {
            if (igBeginTabItem(TR("Curves"), NULL, 0)) {
                if (E.emitter >= 0 && E.emitter < (int)a->nemitters) ch |= curves_panel(&a->e[E.emitter]);
                else igTextDisabled("%s", TR("Select an emitter to see its curves."));
                igEndTabItem();
            }
            if (igBeginTabItem(TR("Timeline"), NULL, 0)) {
                timeline_panel(a);
                igEndTabItem();
            }
            igEndTabBar();
        }
        igEndChild();
    }
    igEnd();
    if (ch) ps_touch(a), E.unsaved = true; // (the runtime compiles it again: the preview and the level show it)
    if (!open) {
        if (E.unsaved) ps_save(a);
        preview(ed, false);
        E.a = NULL;
    }
}

void particle_editor_close(void) { E.a = NULL; }
