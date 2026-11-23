// HUD modular played (--hud-test, the test hud; notas-dev/hud.md): first hud_selftest, then in a new project, away from
// the template's level (y 6000):
//   * BP_HudPlayer: a Character (first person, no gravity) with Health (100), a HUD Layout (every piece, the minimap too)
//     and a float variable Stamina 40 of MaxStamina 80 (the Stamina bar reads it)
//   * BP_HudTarget ahead (+X, 8 m), with Health: its BeginPlay calls Add Screen Marker, Add Compass Marker, Show HUD Message
//     "+1 Coin" twice and Show Damage Number (critical): the nodes, through a Blueprint
//   * a foe at +Y that hurts the player (the damage's direction), 100 more markers around (half out of the view) and 50
//     damage numbers alive, the HUD timed (CPU) and its GPU cost measured (shown / hidden, alternated)
// Step by step: WBP_GameHUD on the screen; the feed's "+2 Coin"; the target's marker projected ahead; the health bar's
// loss smoothed and its trail lingering then drained; the Stamina bar at 0.5; Bind Attribute Bar on the target; the
// crosshair's spread; Set HUD Visible, the photo mode; the layout's safe area and scale.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "health.h"
#include "hud.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_widget.h"

static Entity g_player, g_target, g_foe;
static int g_state, g_wait = 10;
static double g_until;
static bool g_ok = true, g_done;
static double g_cpu_ms, g_draw_ms, g_on_gpu, g_off_gpu;
static int g_cpu_n, g_on_n, g_off_n, g_phase_frames;
static double g_kind_ms[8];
static uint32_t g_kind_vtx[8];
#define GPU_SAMPLES 4096
static float g_on_s[GPU_SAMPLES], g_off_s[GPU_SAMPLES], g_cpu_s[GPU_SAMPLES];
static uint32_t g_vtx_max, g_markers_seen, g_numbers_min = 9999, g_numbers_sum, g_numbers_n;
static double g_numbers_at;

static const float Y = 6000.0f; // (away from the template's level)

#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: hud: " __VA_ARGS__); printf("\n"); g_ok = false; } } while (0)

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] FAIL: hud: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_ok = false;
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin(g, a, 1, ap), b, (uint8_t)pin(g, b, 0, bpn))) printf("[RealEngine] FAIL: hud: link %s -> %s refused\n", ap, bpn), g_ok = false;
}
static uint32_t node(BpAsset* a, uint16_t kind, float x, float y) {
    BpNode* n = bp_add_node(a, &a->graphs[0], kind, x, y, NULL, NULL);
    if (!n) g_ok = false;
    return n ? n->id : 0;
}
static void lit_s(BpAsset* a, uint32_t id, const char* p, const char* v) {
    BpNode* n = bp_node(&a->graphs[0], id);
    if (n) snprintf(n->pins[pin(&a->graphs[0], id, 0, p)].slit, sizeof n->pins[0].slit, "%s", v);
}
static void lit_f(BpAsset* a, uint32_t id, const char* p, float v) {
    BpNode* n = bp_node(&a->graphs[0], id);
    if (n) n->pins[pin(&a->graphs[0], id, 0, p)].lit.v[0] = v;
}

static void look(World* w, float yaw, float pitch) {
    CCharacter* c = ecs_get(w, g_player, COMP_CHARACTER);
    if (c) c->control_yaw = yaw, c->control_pitch = pitch;
}
static void wait_s(Editor* ed, double s) { g_until = ed->play_time + s; }

static const HudBarState* bar(const char* name) {
    Entity owner = ENTITY_NONE;
    UiWidget* x = ui_live_widget(ENTITY_NONE, name, &owner);
    return x ? hud_bar_state(owner, ui_live_widget_index(owner, x), x, 0.0f) : NULL;
}
static bool has_message(const char* text) {
    const HudMessage* m = hud_messages(NULL);
    for (int i = 0; i < HUD_MSG_MAX; i++)
        if (!strcmp(m[i].text, text)) return true;
    return false;
}

static void measure(Editor* ed) {
    World* w = &ed->world;
    // 50 damage numbers alive: 50 new ones as the last ones end (their life is 1.1 s)
    if (ed->play_time - g_numbers_at > 1.08) {
        g_numbers_at = ed->play_time;
        for (int i = 0; i < 50; i++) hud_damage_number((float[3]){ 500.0f + (float)(i % 10) * 60.0f, Y - 300.0f + (float)(i / 10) * 150.0f, 150.0f }, 10.0f + (float)i, i % 7 == 0, i % 5 == 0);
    }
    const HudStats* st = hud_stats();
    g_phase_frames++;
    bool shown = hud_shown();
    if (g_phase_frames > 5) { // (the first frames after a switch: the GPU's last frame was the other kind)
        float ui = (float)st->ui_gpu_ms; // (the UI pass's timestamps: the HUD's pieces are in it)
        if (shown && g_on_n < GPU_SAMPLES) g_on_s[g_on_n++] = ui, g_on_gpu += ui;
        else if (!shown && g_off_n < GPU_SAMPLES) g_off_s[g_off_n++] = ui, g_off_gpu += ui;
        if (shown && st->pieces) {
            if (g_cpu_n < GPU_SAMPLES) g_cpu_s[g_cpu_n] = (float)(st->draw_ms + st->tick_ms);
            g_cpu_ms += st->draw_ms + st->tick_ms, g_draw_ms += st->draw_ms, g_cpu_n++;
            for (int k = 0; k < 8; k++) g_kind_ms[k] += st->kind_ms[k], g_kind_vtx[k] = st->kind_vtx[k];
            if (st->draw_vtx > g_vtx_max) g_vtx_max = st->draw_vtx;
            if (st->markers > g_markers_seen) g_markers_seen = st->markers;
            g_numbers_sum += st->numbers, g_numbers_n++;
            if (st->numbers < g_numbers_min) g_numbers_min = st->numbers;
        }
    }
    if (g_phase_frames >= 40) hud_set_visible(!shown), g_phase_frames = 0; // (alternated: the GPU with and without the HUD)
    (void)w;
}

static void step(Editor* ed, double dt) {
    World* w = &ed->world;
    (void)dt;
    if (g_done) return;
    if (g_state >= 30) {
        measure(ed);
        return;
    }
    if (g_wait-- > 0 || ed->play_time < g_until) return;
    if (ed->play_time > 25.0) {
        CHECK(false, "stuck at step %d", g_state);
        g_state = 30;
        return;
    }
    switch (g_state) {
    case 0: // the HUD on the screen, the nodes' work
        bp_set_player(w, g_player);
        look(w, 0, 0);
        CHECK(hud_layout_widget() != ENTITY_NONE, "the HUD Layout made no widget");
        CHECK(ui_test_shown("Compass") && ui_test_shown("HealthBar") && ui_test_shown("MessageFeed") && ui_test_shown("Minimap"),
              "WBP_GameHUD's pieces are not on the screen");
        wait_s(ed, 0.4);
        g_state = 1;
        break;
    case 1: {
        CHECK(has_message("+2 Coin"), "the feed has no \"+2 Coin\" (Show HUD Message \"+1 Coin\" twice)");
        uint32_t nm = 0, screen = 0, compass = 0;
        const HudMarker* m = hud_markers(&nm);
        for (uint32_t i = 0; i < nm; i++)
            if (m[i].id && m[i].actor == g_target) screen += (m[i].flags & HUD_MK_SCREEN) != 0, compass += (m[i].flags & HUD_MK_COMPASS) != 0;
        CHECK(screen == 1 && compass == 1, "the target's markers: %u screen, %u compass (1, 1)", screen, compass);
        const CTransform* tt = ecs_get(w, g_target, COMP_TRANSFORM);
        float xy[2], z;
        bool ahead = tt && hud_project(tt->location, 0, 0, 1920, 1080, xy, &z);
        CHECK(ahead && fabsf(xy[0] - 960.0f) < 120.0f, "the target is not ahead on the screen (%.0f)", xy[0]);
        uint32_t hn = 0, crit = 0;
        const HealthNumber* num = health_numbers(&hn);
        for (uint32_t i = 0; i < hn; i++) crit += num[i].critical && num[i].amount > 76.0f && num[i].amount < 78.0f;
        CHECK(crit == 1, "Show Damage Number's critical 77 is not in the numbers");
        const HudBarState* sb = bar("StaminaBar");
        CHECK(sb && fabsf(sb->shown - 0.5f) < 0.02f && fabsf(sb->max - 80.0f) < 0.01f, "the Stamina bar shows %.2f of %.0f (0.5 of 80: the variables)", sb ? sb->shown : -1.0f,
              sb ? sb->max : -1.0f);
        const HudBarState* hb = bar("HealthBar");
        CHECK(hb && hb->shown > 0.99f, "the health bar is not full (%.2f)", hb ? hb->shown : -1.0f);
        health_apply_damage(w, g_player, 40.0f, "", g_foe, g_foe); // (from +Y: the right)
        wait_s(ed, 0.3);
        g_state = 2;
        break;
    }
    case 2: {
        const HudBarState* hb = bar("HealthBar");
        CHECK(hb && fabsf(hb->shown - 0.6f) < 0.04f && hb->trail > 0.95f, "0.3 s after 40 damage the bar is at %.2f (0.6) and its trail at %.2f (still 1)",
              hb ? hb->shown : -1.0f, hb ? hb->trail : -1.0f);
        uint32_t nh = 0, live = 0;
        const HudHit* hh = hud_hits(&nh);
        float rel = 0.0f;
        const CTransform* pt = ecs_get(w, g_player, COMP_TRANSFORM);
        for (uint32_t i = 0; i < nh; i++)
            if (hh[i].age < 1.0f) {
                live++;
                float d[3] = { hh[i].from[0] - pt->location[0], hh[i].from[1] - pt->location[1], 0 };
                rel = hud_bearing(d);
            }
        CHECK(live == 1 && fabsf(rel - 90.0f) < 5.0f, "the damage's direction: %u hits, bearing %.0f (1, 90: east, the right)", live, rel);
        wait_s(ed, 1.4);
        g_state = 3;
        break;
    }
    case 3: {
        const HudBarState* hb = bar("HealthBar");
        CHECK(hb && fabsf(hb->trail - 0.6f) < 0.03f, "1.7 s after the damage the trail is at %.2f (drained to 0.6)", hb ? hb->trail : -1.0f);
        // Bind Attribute Bar: the shield bar shows the target's health (75 of 150)
        CHECK(hud_bind_bar(w, ENTITY_NONE, "ShieldBar", g_target, "Health"), "Bind Attribute Bar refused ShieldBar");
        CHECK(!hud_bind_bar(w, ENTITY_NONE, "NoSuchBar", g_target, "Health"), "Bind Attribute Bar took a bar that isn't there");
        // the crosshair: Set Crosshair Spread 1
        hud_set_spread(1.0f);
        wait_s(ed, 0.5);
        g_state = 4;
        break;
    }
    case 4: {
        const HudBarState* sb = bar("ShieldBar");
        CHECK(sb && fabsf(sb->value - 75.0f) < 0.01f && fabsf(sb->max - 150.0f) < 0.01f, "the bound bar shows %.0f of %.0f (the target's 75 of 150)",
              sb ? sb->value : -1.0f, sb ? sb->max : -1.0f);
        CHECK(hud_spread() > 0.9f, "the crosshair's spread is %.2f (1)", hud_spread());
        hud_set_spread(0.0f);
        // the layout: 2% safe margins, x1.5 scale: the widget's rectangle and scale
        CHudLayout* c = ecs_get(w, g_player, COMP_HUD_LAYOUT);
        if (c) c->safe[0] = c->safe[1] = c->safe[2] = c->safe[3] = 2.0f, c->scale = 1.5f;
        wait_s(ed, 0.1);
        g_state = 5;
        break;
    }
    case 5: {
        float r[4] = { 0, 0, 1920, 1080 }, k = 0.0f;
        bool ok = hud_layout_rect(hud_layout_widget(), r, &k);
        CHECK(ok && fabsf(r[0] - 38.4f) < 0.1f && fabsf(r[3] - 1058.4f) < 0.1f && fabsf(k - 1.5f * 1036.8f / 1080.0f) < 0.01f,
              "the layout's rectangle %.1f..%.1f and scale %.3f (38.4, 1058.4, 1.44)", r[0], r[3], k);
        CHudLayout* c = ecs_get(w, g_player, COMP_HUD_LAYOUT);
        if (c) memset(c->safe, 0, sizeof c->safe), c->scale = 1.0f;
        // Set HUD Visible false: every piece gone; the photo mode too
        hud_set_visible(false);
        wait_s(ed, 0.1);
        g_state = 6;
        break;
    }
    case 6:
        CHECK(!hud_shown() && !ui_test_shown("Compass"), "Set HUD Visible false: the HUD is still on the screen");
        hud_set_visible(true), hud_set_photo_mode(true);
        wait_s(ed, 0.1);
        g_state = 7;
        break;
    case 7:
        CHECK(!hud_shown() && !ui_test_shown("Compass"), "the photo mode: the HUD is still on the screen");
        hud_set_photo_mode(false);
        wait_s(ed, 0.1);
        g_state = 8;
        break;
    case 8: {
        CHECK(hud_shown() && ui_test_shown("Compass"), "the HUD did not come back");
        // the measure: 100 markers around (screen + compass, half behind), 50 damage numbers
        for (int i = 0; i < 100; i++) {
            float a = (float)i / 100.0f * 6.2831853f, r = 600.0f + (float)(i % 5) * 300.0f;
            hud_add_marker(w, ENTITY_NONE, (float[3]){ cosf(a) * r, Y + sinf(a) * r, 150.0f }, i % 3 ? "Loot" : "Objective",
                           (float[4]){ 0.4f + (float)(i % 3) * 0.2f, 0.9f, 0.4f, 1.0f }, 0.0f, HUD_MK_SCREEN | HUD_MK_OFFSCREEN | HUD_MK_DISTANCE | (i % 2 ? HUD_MK_COMPASS : 0u));
        }
        g_numbers_at = -10.0;
        g_state = 30;
        break;
    }
    default: g_state = 30; break;
    }
}

bool hud_test_setup(Editor* ed) {
    if (hud_selftest()) {
        printf("[RealEngine] FAIL: hud: the self-test failed\n");
        return false;
    }
    World* w = &ed->world;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/HudTest", ui_content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    if (!hud_layout_asset()) {
        printf("[RealEngine] FAIL: hud: WBP_GameHUD could not be made\n");
        return false;
    }
    // BP_HudPlayer: a Character with Health, a HUD Layout (every piece) and Stamina 40 / MaxStamina 80
    BpAsset* pc = bp_create(BPA_CLASS, "/Content/HudTest", "BP_HudPlayer", "Character");
    if (!pc) return false;
    pc->comp_mask |= COMP_BIT(COMP_HEALTH) | COMP_BIT(COMP_HUD_LAYOUT);
    memset(pc->comp_data[COMP_HEALTH], 0, sizeof pc->comp_data[COMP_HEALTH]);
    memset(pc->comp_data[COMP_HUD_LAYOUT], 0, sizeof pc->comp_data[COMP_HUD_LAYOUT]);
    ecs_component_defaults(COMP_HEALTH, pc->comp_data[COMP_HEALTH]);
    ecs_component_defaults(COMP_HUD_LAYOUT, pc->comp_data[COMP_HUD_LAYOUT]);
    CHealth* ph = (CHealth*)pc->comp_data[COMP_HEALTH];
    ph->max_health = ph->health = 100.0f, ph->max_shield = ph->shield = 0.0f, ph->regen_rate = ph->shield_regen_rate = 0.0f;
    ((CHudLayout*)pc->comp_data[COMP_HUD_LAYOUT])->pieces = HUD_PIECE_ALL;
    int sv = bp_add_var(pc, "Stamina", BPT_FLOAT), mv = bp_add_var(pc, "MaxStamina", BPT_FLOAT);
    if (sv >= 0) pc->vars[sv].def.v[0] = 40.0f;
    if (mv >= 0) pc->vars[mv].def.v[0] = 80.0f;
    if (!bp_save(pc) || !bp_compile(pc)) {
        printf("[RealEngine] FAIL: hud: BP_HudPlayer does not compile: %s\n", pc->error);
        return false;
    }
    // BP_HudTarget: Health 75 of 150; BeginPlay: its markers, "+1 Coin" twice, a critical 77
    BpAsset* tc = bp_create(BPA_CLASS, "/Content/HudTest", "BP_HudTarget", "Actor");
    if (!tc) return false;
    tc->comp_mask |= COMP_BIT(COMP_HEALTH);
    memset(tc->comp_data[COMP_HEALTH], 0, sizeof tc->comp_data[COMP_HEALTH]);
    ecs_component_defaults(COMP_HEALTH, tc->comp_data[COMP_HEALTH]);
    CHealth* th = (CHealth*)tc->comp_data[COMP_HEALTH];
    th->max_health = 150.0f, th->health = 75.0f, th->max_shield = th->shield = 0.0f, th->regen_rate = th->shield_regen_rate = 0.0f;
    BpGraph* g = &tc->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t bp = node(tc, BPK_EVENT_BEGIN_PLAY, 0, 0), sm = node(tc, BPK_HUD_ADD_SCREEN, 300, 0), cm = node(tc, BPK_HUD_ADD_COMPASS, 600, 0);
    uint32_t m1 = node(tc, BPK_HUD_MESSAGE, 900, 0), m2 = node(tc, BPK_HUD_MESSAGE, 1200, 0), dn = node(tc, BPK_HUD_DAMAGE_NUMBER, 1500, 0);
    uint32_t gl = node(tc, BPK_GET_LOCATION, 1200, 250);
    link(g, bp, "", sm, ""), link(g, sm, "", cm, ""), link(g, cm, "", m1, ""), link(g, m1, "", m2, ""), link(g, m2, "", dn, "");
    link(g, gl, "", dn, "Location");
    lit_s(tc, sm, "Label", "Target"), lit_s(tc, cm, "Label", "Target");
    lit_s(tc, m1, "Text", "+1 Coin"), lit_s(tc, m2, "Text", "+1 Coin");
    lit_f(tc, dn, "Amount", 77.0f), lit_f(tc, dn, "Critical", 1.0f);
    if (!g_ok || !bp_save(tc) || !bp_compile(tc)) {
        printf("[RealEngine] FAIL: hud: BP_HudTarget does not compile: %s\n", tc->error);
        return false;
    }
    // the floor, the player, the target ahead, the foe at its right
    Entity floor = editor_spawn_shape(ed, SHAPE_PLANE, (float[3]){ 0, Y, 0 }, 0.0);
    CTransform* ft = floor ? ecs_get(w, floor, COMP_TRANSFORM) : NULL;
    if (ft) ft->scale[0] = 60.0f, ft->scale[1] = 60.0f, ft->location[2] = -1.0f, ft->flags = MOBILITY_STATIC;
    g_player = bp_spawn(w, pc, (float[3]){ 0, Y, 100 }, (float[3]){ 0, 0, 0 });
    CCharacter* cc = g_player ? ecs_get(w, g_player, COMP_CHARACTER) : NULL;
    if (cc) cc->gravity_scale = 0.0f, cc->camera_distance = 0.0f, cc->auto_possess = 1, cc->max_walk_speed = 300.0f;
    CTransform* pt = g_player ? ecs_get(w, g_player, COMP_TRANSFORM) : NULL;
    if (pt) pt->flags = MOBILITY_MOVABLE;
    g_target = bp_spawn(w, tc, (float[3]){ 800, Y, 100 }, (float[3]){ 0, 0, 0 });
    g_foe = editor_spawn_shape(ed, SHAPE_CUBE, (float[3]){ 0, Y + 900, 50 }, 0.0);
    if (!g_player || !g_target || !g_foe || !cc) {
        printf("[RealEngine] FAIL: hud: the actors\n");
        return false;
    }
    ed->cam.pos[0] = -400.0f, ed->cam.pos[1] = Y - 600.0f, ed->cam.pos[2] = 400.0f;
    ed->cam.yaw = 40.0f, ed->cam.pitch = -25.0f;
    ed->test_step = step;
    editor_play(ed, 0.0);
    return true;
}

static int fcmp(const void* a, const void* b) {
    float x = *(const float*)a, y = *(const float*)b;
    return x < y ? -1 : x > y;
}
static double median(float* v, int n) {
    if (n <= 0) return 0.0;
    qsort(v, (size_t)n, sizeof v[0], fcmp);
    return v[n / 2];
}

bool hud_test_check(Editor* ed) {
    if (g_state < 30) printf("[RealEngine] FAIL: hud: stopped at step %d\n", g_state), g_ok = false;
    const HudStats* st = hud_stats();
    double cpu = g_cpu_n ? g_cpu_ms / g_cpu_n : 0.0, draw = g_cpu_n ? g_draw_ms / g_cpu_n : 0.0;
    double on = median(g_on_s, g_on_n), off = median(g_off_s, g_off_n); // (medians: the GPU is shared with other programs)
    double cpu_med = median(g_cpu_s, g_cpu_n < GPU_SAMPLES ? g_cpu_n : GPU_SAMPLES); // (and the CPU: a frame preempted by another program is not the HUD's)
    printf("[RealEngine] HUD cost: CPU %.4f ms a frame (median; mean %.4f: drawing %.4f, update %.4f; %u markers, %.0f damage numbers on average, min %u), up to %u vertices; "
           "GPU: the UI pass %.4f ms with the HUD, %.4f without (%+.4f ms; medians of %d / %d frames)\n",
           cpu_med, cpu, draw, cpu - draw, g_markers_seen, g_numbers_n ? (double)g_numbers_sum / g_numbers_n : 0.0, g_numbers_min, g_vtx_max, on, off, on - off, g_on_n, g_off_n);
    printf("[RealEngine] HUD: %u messages shown, %u stacked since play started\n", st->shown_messages, st->stacked);
    for (int k = 0; k < 8; k++)
        printf("[RealEngine] HUD piece %s: %.4f ms, %u vertices (one frame)\n", g_ui_kind_names[UIW_ATTR_BAR + k], g_kind_ms[k] / (g_cpu_n ? g_cpu_n : 1), g_kind_vtx[k]);
    CHECK(g_cpu_n > 20, "the measure ran %d frames", g_cpu_n);
    CHECK(g_markers_seen >= 102, "the HUD saw %u markers (102)", g_markers_seen);
    CHECK(g_numbers_n && (double)g_numbers_sum / g_numbers_n >= 45.0, "%.1f damage numbers alive on average (50)", g_numbers_n ? (double)g_numbers_sum / g_numbers_n : 0.0);
    CHECK(cpu_med < 0.2, "the HUD costs %.4f ms of CPU a frame (median; 0.2 at most)", cpu_med);
    CHECK(g_on_n > 20 && g_off_n > 20 && on - off < 0.2, "the HUD costs %+.3f ms of GPU a frame (0.2 at most)", on - off);
    editor_stop(ed, 0.0);
    g_done = true;
    if (g_ok) printf("[RealEngine] HUD test: OK\n");
    return g_ok;
}
