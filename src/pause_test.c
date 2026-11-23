// Pause and options (--pause-test, ctest "pause_options"; notas-dev/pausa-opcoes.md). In Play: a box with physics
// falling, two Blueprint actors that move up every Tick (one with Tick When Paused), IMC_Default on. Then, as a player
// would: the pause menu opened (the Play's key), the keyboard down + Enter on it (Options opens), the world checked
// frozen while the UI answers, the preset set to Low in the Options' combo (applied to the view), Jump remapped to F in
// its Key Selector, the master volume and Invert Y changed, Apply (saved); Back (the Options close), the key again (the
// game on). Checked: the box and the plain actor waited, the other moved; F is Jump now, Space isn't; the file written.
// --pause-test-reload (the next run, "pause_options_reload"): a new process reads the file at Play: the same options.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "audio.h"
#include "bp_internal.h"
#include "components.h"
#include "content.h"
#include "editor.h"
#include "game_settings.h"
#include "upscale.h"
#include "input.h"
#include "physics.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "ui_widget.h"

static bool g_fail;
static Entity g_box, g_mover, g_ticker; // (g_ticker: Tick When Paused)
static double g_t;
static int g_step;
static float g_box_z, g_mover_z, g_ticker_z, g_box_z_resumed;
static bool g_jump_f, g_jump_space, g_saved, g_options_shown, g_menu_shown, g_options_back, g_frozen_ok;
static double g_run_ms, g_paused_ms, g_ui_ms;
static uint32_t g_run_n, g_paused_n;
static int g_scal_before = -1;

#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: pause test: " __VA_ARGS__); printf("\n"); g_fail = true; } } while (0)

static float zof(Editor* ed, Entity e) {
    const CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    return t ? t->location[2] : -1e9f;
}

static bool jump_on(void) {
    float v[3];
    uint32_t ev;
    return input_action_state("IA_Jump", v, &ev, NULL, NULL) && v[0] > 0.5f;
}

static void step(Editor* ed, double dt) {
    World* w = &ed->world;
    g_t += dt;
    if (g_step >= 1 && g_step < 9) { // (the measure: the Play's game step running and paused)
        if (game_paused()) g_paused_ms += g_pie_tick_ms, g_paused_n++;
        else g_run_ms += g_pie_tick_ms, g_run_n++;
    }
    switch (g_step) {
    case 0: // the first frame of Play: the Enhanced Input context
        input_add_context("IMC_Default", 0);
        g_step = 1;
        break;
    case 1: // 0.7 s of play: the box falling, the actors moving; then the pause menu (the Play's key)
        if (g_t < 0.7) break;
        pause_menu_key(w, false); // (this frame's step already began: the world waits from the next; the key in a real
                                  // frame comes before it, shortcuts / Esc)
        g_menu_shown = pause_menu_is_open() && game_paused() && ui_test_shown("ResumeButton");
        CHECK(g_menu_shown, "the pause menu didn't open (open %d, paused %d)", pause_menu_is_open(), game_paused());
        g_step = 2, g_t = 0.0;
        break;
    case 2: // the keyboard: down (Resume -> Options), then Enter
        if (g_box_z == 0.0f) g_box_z = zof(ed, g_box), g_mover_z = zof(ed, g_mover), g_ticker_z = zof(ed, g_ticker);
        if (g_t > 0.15) ui_test_nav(1), g_step = 3, g_t = 0.0;
        break;
    case 3:
        if (g_t > 0.15) ui_test_nav(4), g_step = 4, g_t = 0.0;
        break;
    case 4: // the Options shown: the Audio tab, the preset to Low, Jump on F, the volume, Invert Y, then Apply
        if (g_t < 0.2) break;
        g_options_shown = ui_test_shown("GraphicsTab") && ui_test_shown("GraphicsPage");
        CHECK(g_options_shown, "the Options didn't open from the menu's keyboard (Down, Enter)");
        ui_test_click(w, "AudioTab");
        CHECK(ui_test_shown("AudioPage") && !ui_test_shown("GraphicsPage"), "the Audio tab didn't show its page");
        g_scal_before = ed->scalability;
        CHECK(ui_test_bound(w, "Scalability", 0.0f, NULL), "no Quality Preset combo bound to Scalability");
        CHECK(ui_test_bound(w, "Key:IA_Jump:Space", 0.0f, "F"), "no Key Selector bound to Jump's Space");
        CHECK(ui_test_bound(w, "MasterVolume", 0.35f, NULL), "no slider bound to MasterVolume");
        CHECK(ui_test_bound(w, "InvertY", 1.0f, NULL), "no check box bound to InvertY");
        CHECK(ui_test_bound(w, "Upscaler", 1.0f, NULL), "no Upscaler combo bound to Upscaler"); // (4.4a: the 2nd choice, TSR: always there)
        g_step = 5, g_t = 0.0;
        break;
    case 5: // the UI answered while the world waited
        if (g_t < 0.4) break;
        g_ui_ms = ui_stats()->draw_ms;
        CHECK(ed->scalability == 0 && !ed->gi.probes, "the Low preset wasn't applied to the view (scalability %d)", ed->scalability);
        CHECK(fabsf(audio_master_volume() - 0.35f) < 0.01f, "the master volume is %.2f, not 0.35", audio_master_volume());
        CHECK(!strcmp(gus_action_key("IA_Jump", false), "F"), "Jump's key is %s, not F", gus_action_key("IA_Jump", false));
        {
            float bz = zof(ed, g_box), mz = zof(ed, g_mover), tz = zof(ed, g_ticker);
            g_frozen_ok = fabsf(bz - g_box_z) < 0.01f && fabsf(mz - g_mover_z) < 0.01f && tz > g_ticker_z + 5.0f;
            CHECK(fabsf(bz - g_box_z) < 0.01f, "the box with physics moved while paused (%.2f -> %.2f)", g_box_z, bz);
            CHECK(fabsf(mz - g_mover_z) < 0.01f, "the actor without Tick When Paused moved while paused (%.2f -> %.2f)", g_mover_z, mz);
            CHECK(tz > g_ticker_z + 5.0f, "the Tick When Paused actor didn't tick while paused (%.2f -> %.2f)", g_ticker_z, tz);
        }
        ui_test_click(w, "ApplyButton");
        g_saved = re_file_stat(gus_path(), NULL, NULL, NULL);
        CHECK(g_saved, "Apply didn't save %s", gus_path());
        g_step = 6, g_t = 0.0;
        break;
    case 6: // the key: Back first (the Options close, the menu stays), then the menu closes
        if (g_t < 0.15) break;
        pause_menu_key(w, false);
        g_options_back = !ui_test_shown("GraphicsTab") && pause_menu_is_open() && game_paused();
        CHECK(g_options_back, "the key didn't go back from the Options to the menu");
        g_step = 7, g_t = 0.0;
        break;
    case 7:
        if (g_t < 0.15) break;
        pause_menu_key(w, false);
        CHECK(!pause_menu_is_open() && !game_paused() && !ui_test_shown("ResumeButton"), "the menu didn't close (the game on)");
        g_step = 8, g_t = 0.0;
        break;
    case 8: // the game on: the box falls again; F is Jump, Space isn't
        if (g_t < 0.3) break;
        g_box_z_resumed = zof(ed, g_box);
        bp_key_event(w, 'F', true);
        g_step = 9, g_t = 0.0;
        break;
    case 9:
        if (g_t < 0.05) break;
        g_jump_f = jump_on();
        bp_key_event(w, 'F', false), bp_key_event(w, ' ', true);
        g_step = 10, g_t = 0.0;
        break;
    case 10:
        if (g_t < 0.05) break;
        g_jump_space = jump_on();
        bp_key_event(w, ' ', false);
        g_step = 11;
        break;
    default: break;
    }
}

// A Blueprint that moves itself up 1 cm a Tick
static BpAsset* mover_class(void) {
    BpAsset* a = bp_load("/Content/PauseTest/BP_PauseMover.rbp");
    if (!a) a = bp_create(BPA_CLASS, "/Content/PauseTest", "BP_PauseMover", "Actor");
    if (!a) return NULL;
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    BpNode* tick = bp_add_node(a, g, BPK_EVENT_TICK, 0, 0, NULL, NULL);
    BpNode* off = bp_add_node(a, g, BPK_ADD_OFFSET, 300, 0, NULL, NULL);
    if (!tick || !off) return NULL;
    uint32_t tid = tick->id, oid = off->id;
    off = bp_node(g, oid);
    for (int i = 0; i < off->npins; i++)
        if (!off->pins[i].out && !strcmp(off->pins[i].name, "Delta Location")) off->pins[i].lit.v[2] = 1.0f;
    bp_link(g, tid, 0, oid, 0);
    return bp_save(a) && bp_compile(a) ? a : NULL;
}

bool pause_test_setup(Editor* ed, bool reload) {
    World* w = &ed->world;
    if (reload) { // the next run: the file is read when Play starts
        editor_play(ed, 0.0);
        return true;
    }
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/PauseTest", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    remove(gus_path()[0] ? gus_path() : "");
    { // (the project's options file from an earlier run: gone, the defaults stand)
        const char* env = getenv("REALENGINE_SETTINGS_FILE");
        if (env) remove(env);
    }
    BpAsset* cls = mover_class();
    if (!cls) return printf("[RealEngine] FAIL: pause test: BP_PauseMover\n"), false;
    g_mover = bp_spawn(w, cls, (float[3]){ 0, 2600, 100 }, (float[3]){ 0, 0, 0 });
    g_ticker = bp_spawn(w, cls, (float[3]){ 300, 2600, 100 }, (float[3]){ 0, 0, 0 });
    CBlueprint* tb = ecs_get(w, g_ticker, COMP_BLUEPRINT);
    if (tb) tb->flags |= BP_FLAG_TICK_WHEN_PAUSED; // (Details > Blueprint > Tick When Paused)
    g_box = editor_spawn_shape(ed, SHAPE_CUBE, (float[3]){ 600, 2600, 2500 }, 0.0);
    CTransform* t = ecs_get(w, g_box, COMP_TRANSFORM);
    if (t) t->scale[0] = t->scale[1] = t->scale[2] = 0.3f, t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
    if (ecs_add(w, g_box, COMP_RIGID_BODY)) {
        CRigidBody* rb = ecs_get(w, g_box, COMP_RIGID_BODY);
        rb->simulate = 1, rb->enable_gravity = 1, rb->mass = 10.0f, rb->friction = 0.5f;
    }
    ed->cam.pos[0] = 300.0f, ed->cam.pos[1] = 1300.0f, ed->cam.pos[2] = 600.0f, ed->cam.yaw = 90.0f, ed->cam.pitch = -10.0f;
    ed->test_step = step;
    editor_play(ed, 0.0);
    return g_mover && g_ticker && g_box;
}

bool pause_test_check(Editor* ed, bool reload) {
    if (reload) { // what the first run saved, read back when this Play began (and applied)
        const GameUserSettings* s = gus();
        bool ok = s->scalability == 0 && fabsf(s->volume[GUS_VOL_MASTER] - 0.35f) < 0.01f && s->invert_y &&
                  !strcmp(gus_action_key("IA_Jump", false), "F") && fabsf(audio_master_volume() - 0.35f) < 0.01f && s->upscaler == UPS_TSR;
        printf("[RealEngine] Pause reload: %s: preset %d, master %.2f, Invert Y %d, Jump on %s, upscaler %d (applied volume %.2f)\n", gus_path(),
               s->scalability, s->volume[GUS_VOL_MASTER], s->invert_y, gus_action_key("IA_Jump", false), s->upscaler, audio_master_volume());
        if (!ok) printf("[RealEngine] FAIL: pause test: the saved options weren't read back\n");
        else printf("[RealEngine] Pause reload: OK\n");
        remove(gus_path()); // (the next suite starts from the defaults)
        return ok;
    }
    CHECK(g_step == 11, "the steps stopped at %d", g_step);
    CHECK(g_box_z_resumed < g_box_z - 1.0f, "the box didn't fall again after the menu closed (%.2f -> %.2f)", g_box_z, g_box_z_resumed);
    CHECK(g_jump_f, "F doesn't trigger IA_Jump after the remap");
    CHECK(!g_jump_space, "Space still triggers IA_Jump after the remap");
    CHECK(gus_remap_hits() > 0, "Enhanced Input never read a remapped key");
    printf("[RealEngine] Pause cost: game step %.3f ms running, %.3f ms paused (%u / %u frames); options menu drawn in %.3f ms; preset %d -> %d\n",
           g_run_n ? g_run_ms / g_run_n : 0.0, g_paused_n ? g_paused_ms / g_paused_n : 0.0, g_run_n, g_paused_n, g_ui_ms, g_scal_before, ed->scalability);
    if (!g_fail) printf("[RealEngine] Pause test: OK (menu, frozen world, Options by keyboard, preset, remap, saved)\n");
    return !g_fail;
}
