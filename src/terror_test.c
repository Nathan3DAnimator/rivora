// The horror kit's tests (Q13, notas-dev/terror.md): --scene terror_filters / terror_exposure / terror_channels --bench N.
// Each one reads the viewport back (VkCtx grab: the picture as it goes to the screen) and checks what it shows.
//   terror_filters:  each filter at full on a lit scene with the kit's clock held: two reads a few frames apart are the
//                    same (determinism: the noise follows the clock, not the frame), each looks as it should (White
//                    Flash white, Black & White grey and bright, Hallucination red, TV Static noise that changes with the
//                    clock...), the letterbox's bars black; and the whole stack's cost on the GPU (composite with every
//                    filter on, minus off; scaled to 1440p): under 0.3 ms
//   terror_exposure: a closed dark room, one small lamp, auto exposure: the usual metering lifts the dark part, the horror
//                    exposure (Post Process Volume > Horror) keeps it black and the lamp lit
//   terror_channels: a spot on Lighting Channel 1 over two cubes, one on channel 1 and one on channel 0: the first lit,
//                    the second exactly as dark as with the light off (no leak); on channel 0 the light reaches it
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "post.h"
#include "render.h"
#include "terror.h"
#include "terror_test.h"
#include "vkctx.h"

// (the picture without the viewport's own overlays: its toolbar on top, the stats in the corners)
#define RX0 0.1f
#define RY0 0.15f
#define RX1 0.9f
#define RY1 0.9f
typedef struct Shot { float mean[3], lum, sat, sd; uint64_t hash; uint32_t w, h; } Shot;

static struct {
    int scene; // 1 filters, 2 exposure, 3 channels
    float t;   // the test's own seconds
    int step, wait;
    bool asked, failed;
    uint8_t* px;
    uint32_t pw, ph;
    Shot base, a, b;
    // filters
    int filter;
    float filter_check[TF_COUNT][4];
    double cost_on, cost_off;
    int cost_n_on, cost_n_off;
    // exposure / channels
    Entity light, cube_a, cube_b, ppv;
    float dark_off, dark_on, lamp_on, lamp_off;
    float a_on, a_off, b_on, b_off, b_ch0;
} TT;

static float clamp01(float v) { return v < 0.0f ? 0.0f : v > 1.0f ? 1.0f : v; }

static Entity mesh(World* w, int shape, const float at[3], const float scale[3], const float color[3]) {
    Entity e = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_NAME));
    if (e == ENTITY_NONE) return e;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12), memcpy(t->scale, scale, 12);
    t->rotation[0] = t->rotation[1] = t->rotation[2] = 0.0f;
    t->flags = MOBILITY_STATIC;
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    m->mesh = (uint32_t)shape;
    memcpy(m->base_color, color, 12);
    m->roughness = 0.7f;
    m->cast_shadow = 1;
    return e;
}
static Entity light(World* w, int type, const float at[3], float pitch, float yaw, float intensity) {
    Entity e = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT) | COMP_BIT(COMP_NAME));
    if (e == ENTITY_NONE) return e;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, at, 12);
    t->rotation[0] = pitch, t->rotation[1] = yaw;
    t->flags = MOBILITY_MOVABLE;
    CLight* l = ecs_get(w, e, COMP_LIGHT);
    l->type = (uint32_t)type, l->intensity = intensity;
    return e;
}
static Entity volume(World* w) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_POST_PROCESS));
    CPostProcess* p = e ? ecs_get(w, e, COMP_POST_PROCESS) : NULL;
    if (p) post_volume_defaults(p);
    return e;
}

bool terror_test_scene(Editor* ed, const char* name) {
    World* w = &ed->world;
    memset(&TT, 0, sizeof TT);
    TT.scene = !strcmp(name, "terror_filters") ? 1 : !strcmp(name, "terror_exposure") ? 2 : !strcmp(name, "terror_channels") ? 3 : 0;
    if (!TT.scene) return false;
    for (uint32_t k = 1; k < w->slot_count; k++) { // (the editor's start level out: its sun, sky and fog would light the rooms)
        Entity e = ecs_entity_at(w, k);
        if (e) ecs_destroy(w, e);
    }
    terror_reset();
    terror_set_time_frozen(true, 12.345); // (the noise's clock held: the same picture for the same time)
    if (TT.scene == 1) { // a lit floor, colored shapes, the sun and the sky
        mesh(w, SHAPE_PLANE, (float[3]){ 0, 0, 0 }, (float[3]){ 40, 40, 1 }, (float[3]){ 0.5f, 0.5f, 0.5f });
        mesh(w, SHAPE_CUBE, (float[3]){ 600, -250, 100 }, (float[3]){ 2, 2, 2 }, (float[3]){ 0.8f, 0.15f, 0.1f });
        mesh(w, SHAPE_SPHERE, (float[3]){ 650, 0, 100 }, (float[3]){ 2, 2, 2 }, (float[3]){ 0.1f, 0.6f, 0.2f });
        mesh(w, SHAPE_CYLINDER, (float[3]){ 600, 250, 100 }, (float[3]){ 2, 2, 2 }, (float[3]){ 0.15f, 0.25f, 0.8f });
        light(w, LIGHT_DIRECTIONAL, (float[3]){ 0, 0, 500 }, -45.0f, 30.0f, 10.0f);
        Entity sky = light(w, LIGHT_SKY, (float[3]){ 0, 0, 450 }, 0, 0, 1.0f);
        (void)sky;
        ed->cam.pos[0] = -100.0f, ed->cam.pos[1] = 0.0f, ed->cam.pos[2] = 180.0f, ed->cam.yaw = 0.0f, ed->cam.pitch = -8.0f;
        ed->auto_exposure = false, ed->exposure_ev100 = 3.0f; // (held: the reads compare the filters, not the exposure adapting)
    } else if (TT.scene == 2) { // a closed room 8 x 8 x 3 m, one small lamp in the far right corner
        const float g[3] = { 0.45f, 0.43f, 0.4f };
        mesh(w, SHAPE_CUBE, (float[3]){ 0, 0, -10 }, (float[3]){ 8.4f, 8.4f, 0.2f }, g);
        mesh(w, SHAPE_CUBE, (float[3]){ 0, 0, 310 }, (float[3]){ 8.4f, 8.4f, 0.2f }, g);
        mesh(w, SHAPE_CUBE, (float[3]){ 410, 0, 150 }, (float[3]){ 0.2f, 8.4f, 3.2f }, g);
        mesh(w, SHAPE_CUBE, (float[3]){ -410, 0, 150 }, (float[3]){ 0.2f, 8.4f, 3.2f }, g);
        mesh(w, SHAPE_CUBE, (float[3]){ 0, 410, 150 }, (float[3]){ 8.4f, 0.2f, 3.2f }, g);
        mesh(w, SHAPE_CUBE, (float[3]){ 0, -410, 150 }, (float[3]){ 8.4f, 0.2f, 3.2f }, g);
        TT.light = light(w, LIGHT_POINT, (float[3]){ 330, 300, 60 }, 0, 0, 40.0f);
        CLight* l = ecs_get(w, TT.light, COMP_LIGHT);
        l->attenuation_radius = 450.0f, l->color[1] = 0.7f, l->color[2] = 0.45f, l->source_radius = 2.0f;
        TT.ppv = volume(w);
        ed->auto_exposure = true, ed->auto_min_ev100 = -6.0f, ed->auto_max_ev100 = 16.0f, ed->exposure_comp = 0.0f;
        ed->cam.pos[0] = -350.0f, ed->cam.pos[1] = -300.0f, ed->cam.pos[2] = 160.0f, ed->cam.yaw = 42.0f, ed->cam.pitch = -10.0f;
    } else { // two cubes side by side 3 m ahead in a closed room, a spot over both on Lighting Channel 1, nothing else lit
        const float g[3] = { 0.3f, 0.3f, 0.3f };
        mesh(w, SHAPE_CUBE, (float[3]){ 0, 0, -80 }, (float[3]){ 12.4f, 12.4f, 0.2f }, g);
        mesh(w, SHAPE_CUBE, (float[3]){ 0, 0, 400 }, (float[3]){ 12.4f, 12.4f, 0.2f }, g);
        mesh(w, SHAPE_CUBE, (float[3]){ 610, 0, 160 }, (float[3]){ 0.2f, 12.4f, 5.0f }, g);
        mesh(w, SHAPE_CUBE, (float[3]){ -610, 0, 160 }, (float[3]){ 0.2f, 12.4f, 5.0f }, g);
        mesh(w, SHAPE_CUBE, (float[3]){ 0, 610, 160 }, (float[3]){ 12.4f, 0.2f, 5.0f }, g);
        mesh(w, SHAPE_CUBE, (float[3]){ 0, -610, 160 }, (float[3]){ 12.4f, 0.2f, 5.0f }, g);
        TT.cube_a = mesh(w, SHAPE_CUBE, (float[3]){ 300, -80, 0 }, (float[3]){ 1.2f, 1.2f, 1.2f }, (float[3]){ 0.7f, 0.7f, 0.7f });
        TT.cube_b = mesh(w, SHAPE_CUBE, (float[3]){ 300, 80, 0 }, (float[3]){ 1.2f, 1.2f, 1.2f }, (float[3]){ 0.7f, 0.7f, 0.7f });
        ((CStaticMesh*)ecs_get(w, TT.cube_a, COMP_STATIC_MESH))->light_channels = 2u; // channel 1 only
        TT.light = light(w, LIGHT_SPOT, (float[3]){ 0, 0, 250 }, -38.0f, 0.0f, 3000.0f);
        CLight* l = ecs_get(w, TT.light, COMP_LIGHT);
        l->light_channels = 2u, l->outer_cone = 60.0f, l->inner_cone = 40.0f, l->attenuation_radius = 1500.0f;
        l->indirect_intensity = 0.0f, l->cast_shadows = 0; // (its bounce isn't direct light: channels are the direct light's, as in Unreal)
        ed->auto_exposure = false, ed->exposure_ev100 = 6.0f;
        ed->cam.pos[0] = -150.0f, ed->cam.pos[1] = 0.0f, ed->cam.pos[2] = 0.0f, ed->cam.yaw = 0.0f, ed->cam.pitch = 0.0f;
    }
    printf("[RealEngine] Horror kit test %s: built\n", name);
    return true;
}

// The last grab's statistics over a rectangle (shares of the viewport)
static Shot stats_rect(float x0, float y0, float x1, float y1) {
    Shot s;
    memset(&s, 0, sizeof s);
    if (!TT.px || !TT.pw || !TT.ph) return s;
    uint32_t ax = (uint32_t)(x0 * (float)TT.pw), bx = (uint32_t)(x1 * (float)TT.pw), ay = (uint32_t)(y0 * (float)TT.ph), by = (uint32_t)(y1 * (float)TT.ph);
    if (bx > TT.pw) bx = TT.pw;
    if (by > TT.ph) by = TT.ph;
    double sum[3] = { 0, 0, 0 }, ssat = 0.0, sl = 0.0, sl2 = 0.0, n = 0.0;
    uint64_t h = 1469598103934665603ull;
    for (uint32_t y = ay; y < by; y++)
        for (uint32_t x = ax; x < bx; x++) {
            const uint8_t* p = TT.px + ((size_t)y * TT.pw + x) * 3;
            float r = p[0] / 255.0f, g = p[1] / 255.0f, b = p[2] / 255.0f;
            float l = 0.2126f * r + 0.7152f * g + 0.0722f * b, mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b));
            sum[0] += r, sum[1] += g, sum[2] += b, sl += l, sl2 += (double)l * l, ssat += mx > 1e-3f ? (mx - mn) / mx : 0.0f, n += 1.0;
            h = (h ^ p[0] ^ (uint64_t)p[1] << 8 ^ (uint64_t)p[2] << 16) * 1099511628211ull;
        }
    if (n <= 0.0) return s;
    for (int k = 0; k < 3; k++) s.mean[k] = (float)(sum[k] / n);
    s.lum = (float)(sl / n), s.sat = (float)(ssat / n), s.sd = (float)sqrt(fmax(sl2 / n - (sl / n) * (sl / n), 0.0)), s.hash = h;
    s.w = bx - ax, s.h = by - ay;
    return s;
}
// the mean absolute difference of two grabs (the last one against a copy)
static uint8_t *g_keep, *g_base;
// the mean absolute difference of the last grab and a copy, over the region without the overlays
static float diff_region(const uint8_t* other) {
    if (!other || !TT.px) return 1.0f;
    double d = 0.0, n = 0.0;
    for (uint32_t y = (uint32_t)(RY0 * TT.ph); y < (uint32_t)(RY1 * TT.ph); y++)
        for (uint32_t x = (uint32_t)(RX0 * TT.pw); x < (uint32_t)(RX1 * TT.pw); x++)
            for (int k = 0; k < 3; k++) {
                size_t i = ((size_t)y * TT.pw + x) * 3 + (size_t)k;
                d += abs((int)TT.px[i] - (int)other[i]), n += 1.0;
            }
    return n > 0.0 ? (float)(d / n / 255.0) : 1.0f;
}
static float diff_kept(void) { return diff_region(g_keep); }
static void keep_into(uint8_t** dst) {
    size_t n = (size_t)TT.pw * TT.ph * 3;
    uint8_t* k = realloc(*dst, n); // (a test's: not a frame of the engine's)
    if (k) *dst = k, memcpy(k, TT.px, n);
}
static void keep(void) { keep_into(&g_keep); }

// a grab asked and taken: true once the pixels are in TT.px
static bool grab(Editor* ed, VkCtx* vk) {
    if (!TT.asked) {
        const RenderView* rv = &ed->render_view;
        vk->grab_rect[0] = rv->x, vk->grab_rect[1] = rv->y, vk->grab_rect[2] = rv->w, vk->grab_rect[3] = rv->h;
        vk->grab_ready = false, vk->grab_req = true;
        TT.asked = true;
        return false;
    }
    if (!vk->grab_ready) return false;
    vk->grab_ready = false, TT.asked = false;
    size_t n = (size_t)vk->grab_w * vk->grab_h * 3;
    uint8_t* p = realloc(TT.px, n);
    if (!p) return false;
    TT.px = p, memcpy(TT.px, vk->grab_px, n), TT.pw = vk->grab_w, TT.ph = vk->grab_h;
    return true;
}

static void fail(const char* what, float got, float want) {
    printf("[RealEngine] FAIL: horror kit: %s (%.4f, wanted %.4f)\n", what, got, want);
    TT.failed = true;
}

static void filters_tick(Editor* ed, VkCtx* vk, float dt) {
    terror_tick(&ed->world, dt, true); // (the held filters reach their value; the clock stays held)
    switch (TT.step) {
    case 0: if (TT.t > 3.5f) TT.step = 1; break; // (the GI settles)
    case 1:
        if (grab(ed, vk)) TT.base = stats_rect(RX0, RY0, RX1, RY1), keep_into(&g_base), TT.step = 2, TT.filter = 0;
        break;
    case 2: // a filter at full, held
        terror_filter_clear(0.0f);
        terror_filter_set(TT.filter, 1.0f, 0.0f);
        TT.wait = 4, TT.step = 3;
        break;
    case 3:
        if (--TT.wait <= 0 && grab(ed, vk)) TT.a = stats_rect(RX0, RY0, RX1, RY1), keep(), TT.wait = 5, TT.step = 4;
        break;
    case 4:
        if (--TT.wait <= 0 && grab(ed, vk)) {
            TT.b = stats_rect(RX0, RY0, RX1, RY1);
            float d = diff_kept();
            TT.filter_check[TT.filter][0] = d;
            const char* fn = terror_filter_name(TT.filter);
            printf("[RealEngine] Horror filter %-24s: mean %.3f %.3f %.3f, luminance %.3f (base %.3f), saturation %.3f, sd %.3f; two reads %.5f apart, %.4f from the scene\n", fn,
                   TT.a.mean[0], TT.a.mean[1], TT.a.mean[2], TT.a.lum, TT.base.lum, TT.a.sat, TT.a.sd, d, diff_region(g_base));
            // the same clock, the same picture (the scene's own noise under 1/255 on average)
            if (d > 0.006f) fail("a filter's picture changed with the clock held (not deterministic)", d, 0.006f);
            float dbase = diff_region(g_base); // (the last grab is B: the filter's picture against the scene's)
            switch (TT.filter) {
            case TF_HALLUCINATION:
                if (!(TT.a.mean[0] > 1.25f * TT.a.mean[1] && TT.a.mean[0] > 1.25f * TT.a.mean[2])) fail("Hallucination isn't red", TT.a.mean[0] / fmaxf(TT.a.mean[1], 1e-3f), 1.25f);
                break;
            case TF_BW_BLOWN:
                if (TT.a.sat > 0.03f) fail("Black & White Blown Out keeps color", TT.a.sat, 0.03f);
                if (TT.a.lum < TT.base.lum + 0.1f) fail("Black & White Blown Out isn't blown out", TT.a.lum, TT.base.lum + 0.1f);
                break;
            case TF_WHITE_FLASH:
                if (TT.a.lum < 0.97f) fail("White Flash isn't white", TT.a.lum, 0.97f);
                break;
            case TF_TV_STATIC:
                if (d > 2e-5f) fail("TV Static changed with the clock held", d, 2e-5f);
                if (TT.a.sd < 0.18f) fail("TV Static has no snow", TT.a.sd, 0.18f);
                if (TT.a.sat > 0.03f) fail("TV Static has color", TT.a.sat, 0.03f);
                break;
            default:
                if (dbase < 0.005f) fail("the filter changes nothing", dbase, 0.005f); // (12x the two reads' noise)
                break;
            }
            if (TT.filter == TF_TV_STATIC) { // the clock moves: the snow moves
                terror_set_time_frozen(true, 12.345 + 0.1);
                TT.step = 5, TT.wait = 4;
                break;
            }
            TT.step = ++TT.filter < TF_COUNT ? 2 : 6;
        }
        break;
    case 5:
        if (--TT.wait <= 0 && grab(ed, vk)) {
            float moved = diff_kept();
            printf("[RealEngine] Horror filter TV Static: %.4f apart with the clock 0.1 s on\n", moved);
            if (moved < 0.05f) fail("TV Static doesn't move with the clock", moved, 0.05f);
            terror_set_time_frozen(true, 12.345);
            TT.step = ++TT.filter < TF_COUNT ? 2 : 6;
        }
        break;
    case 6: // the letterbox (2.39:1, at once): its rows black, the picture between
        terror_filter_clear(0.0f);
        terror_set_letterbox(2.39f, 0.0f);
        TT.wait = 4, TT.step = 7;
        break;
    case 7:
        if (--TT.wait <= 0 && grab(ed, vk)) {
            float bar = terror_letterbox();
            Shot top = stats_rect(RX0, fminf(1.0f - bar + 0.01f, 0.999f), RX1, 1.0f), mid = stats_rect(RX0, 0.4f, RX1, 0.6f); // (the bottom bar: no overlay there)
            printf("[RealEngine] Horror letterbox 2.39: bars %.1f%% of the height each, their luminance %.4f, the picture's %.3f\n", bar * 100.0f, top.lum, mid.lum);
            if (bar < 0.05f || top.lum > 0.002f) fail("the letterbox's bars aren't black", top.lum, 0.002f);
            if (mid.lum < 0.02f) fail("the letterbox hid the picture", mid.lum, 0.02f);
            terror_set_letterbox(0.0f, 0.0f);
            TT.step = 8, TT.wait = 30;
        }
        break;
    case 8: // the cost: every filter off, then every one at full, 120 frames each (the composite's GPU time)
        if (--TT.wait > 0) break;
        if (TT.cost_n_off < 120) {
            TT.cost_off += render_stats()->gpu_composite_ms, TT.cost_n_off++;
            if (TT.cost_n_off == 120) {
                for (int f = 0; f < TF_COUNT; f++) terror_filter_set(f, 1.0f, 0.0f);
                terror_set_tension(1.0f, 0.0f), terror_set_letterbox(2.2f, 0.0f);
                const float k[3] = { 0, 0, 0 };
                terror_dip(k, 0.0f, 100.0f, 0.0f);
                TT.wait = 10;
            }
        } else if (TT.cost_n_on < 120) {
            TT.cost_on += render_stats()->gpu_composite_ms, TT.cost_n_on++;
        } else {
            double off = TT.cost_off / TT.cost_n_off, on = TT.cost_on / TT.cost_n_on;
            const RenderView* rv = &ed->render_view;
            double scale = (2560.0 * 1440.0) / fmax((double)rv->w * rv->h, 1.0), at1440 = (on - off) * scale;
            printf("[RealEngine] Horror filter stack: composite %.3f ms off, %.3f ms with every filter, the tension, the letterbox and a dip "
                   "(%.0fx%.0f): the stack %.3f ms, %.3f ms at 1440p\n", off, on, rv->w, rv->h, on - off, at1440);
            if (at1440 > 0.3) fail("the horror filter stack costs over 0.3 ms at 1440p", (float)at1440, 0.3f);
            terror_filter_clear(0.0f), terror_reset();
            TT.step = 99;
        }
        break;
    default: break;
    }
}

static void exposure_tick(Editor* ed, VkCtx* vk, float dt) {
    (void)dt;
    CPostProcess* p = TT.ppv ? ecs_get(&ed->world, TT.ppv, COMP_POST_PROCESS) : NULL;
    switch (TT.step) {
    case 0:
        if (TT.t > 4.0f && grab(ed, vk)) { // the usual auto exposure, settled
            TT.dark_off = stats_rect(0.02f, 0.3f, 0.35f, 0.9f).lum, TT.lamp_off = stats_rect(0.0f, 0.0f, 1.0f, 1.0f).lum;
            if (p) p->terror_flags = TERROR_PPV_EXPOSURE, p->terror_meter = 0.95f, p->terror_toe = 0.5f, p->terror_ev_floor = 1.0f;
            TT.step = 1, TT.t = 0.0f;
        }
        break;
    case 1:
        if (TT.t > 4.0f && grab(ed, vk)) {
            TT.dark_on = stats_rect(0.02f, 0.3f, 0.35f, 0.9f).lum, TT.lamp_on = stats_rect(0.0f, 0.0f, 1.0f, 1.0f).lum;
            // the lit part: the brightest tenth of the picture's pixels
            uint32_t hist[256] = { 0 }, n = TT.pw * TT.ph;
            for (uint32_t i = 0; i < n; i++) {
                const uint8_t* q = TT.px + (size_t)i * 3;
                hist[(q[0] * 54 + q[1] * 183 + q[2] * 19) >> 8]++;
            }
            uint32_t acc = 0;
            int top = 255;
            for (; top > 0 && acc < n / 100; top--) acc += hist[top];
            printf("[RealEngine] Horror exposure: the dark part %.4f with the usual auto exposure, %.4f with the horror exposure; the picture %.4f -> %.4f; the brightest 1%% from %.3f\n",
                   TT.dark_off, TT.dark_on, TT.lamp_off, TT.lamp_on, top / 255.0f);
            if (!(TT.dark_on <= TT.dark_off * 0.6f)) fail("the horror exposure lifts the dark like the usual one", TT.dark_on, TT.dark_off * 0.6f);
            if (TT.dark_on > 0.08f) fail("the dark room isn't dark with the horror exposure", TT.dark_on, 0.08f);
            if (top / 255.0f < 0.35f) fail("the lamp went dark with the horror exposure", top / 255.0f, 0.35f);
            TT.step = 99;
        }
        break;
    default: break;
    }
}

static void channels_tick(Editor* ed, VkCtx* vk, float dt) {
    (void)dt;
    World* w = &ed->world;
    CLight* l = TT.light ? ecs_get(w, TT.light, COMP_LIGHT) : NULL;
    if (!l) return;
    // the cubes' faces on the screen: x = 0.5 -+ (80 / 450) / 2 x the view's half width (horizontal FOV 90)
    const float ax = 0.5f - 0.5f * 80.0f / 450.0f, bx = 0.5f + 0.5f * 80.0f / 450.0f, hw = 0.03f;
    switch (TT.step) {
    case 0:
        if (TT.t > 2.5f && grab(ed, vk)) {
            TT.a_on = stats_rect(ax - hw, 0.46f, ax + hw, 0.54f).lum, TT.b_on = stats_rect(bx - hw, 0.46f, bx + hw, 0.54f).lum;
            l->intensity = 0.0f, ecs_touch(w, TT.light);
            TT.step = 1, TT.t = 0.0f;
        }
        break;
    case 1:
        if (TT.t > 1.5f && grab(ed, vk)) {
            TT.a_off = stats_rect(ax - hw, 0.46f, ax + hw, 0.54f).lum, TT.b_off = stats_rect(bx - hw, 0.46f, bx + hw, 0.54f).lum;
            l->intensity = 3000.0f, l->light_channels = 1u, ecs_touch(w, TT.light); // (channel 0: the other cube's)
            TT.step = 2, TT.t = 0.0f;
        }
        break;
    case 2:
        if (TT.t > 1.5f && grab(ed, vk)) {
            TT.b_ch0 = stats_rect(bx - hw, 0.46f, bx + hw, 0.54f).lum;
            printf("[RealEngine] Lighting Channels: the channel 1 cube %.4f lit, %.4f dark; the channel 0 cube %.4f under the channel 1 light, %.4f dark, %.4f under a channel 0 light\n",
                   TT.a_on, TT.a_off, TT.b_on, TT.b_off, TT.b_ch0);
            if (TT.a_on < TT.a_off + 0.08f) fail("the channel 1 light doesn't light its cube", TT.a_on, TT.a_off + 0.08f);
            if (fabsf(TT.b_on - TT.b_off) > 2.0f / 255.0f) fail("the channel 1 light leaks onto a channel 0 cube", fabsf(TT.b_on - TT.b_off), 2.0f / 255.0f);
            if (TT.b_ch0 < TT.b_off + 0.08f) fail("a channel 0 light doesn't reach a channel 0 cube", TT.b_ch0, TT.b_off + 0.08f);
            TT.step = 99;
        }
        break;
    default: break;
    }
}

void terror_test_tick(Editor* ed, VkCtx* vk, float dt) {
    if (!TT.scene) return;
    TT.t += dt;
    if (TT.scene == 1) filters_tick(ed, vk, dt);
    else if (TT.scene == 2) exposure_tick(ed, vk, dt);
    else channels_tick(ed, vk, dt);
    (void)clamp01;
}

bool terror_test_check(Editor* ed) {
    (void)ed;
    if (!TT.scene) return true;
    terror_set_time_frozen(false, 0.0);
    if (TT.step != 99) printf("[RealEngine] FAIL: horror kit test didn't finish (step %d): a longer --bench\n", TT.step), TT.failed = true;
    if (!TT.failed) printf("[RealEngine] Horror kit test: PASS\n");
    free(TT.px), TT.px = NULL;
    free(g_keep), g_keep = NULL;
    free(g_base), g_base = NULL;
    return !TT.failed;
}
