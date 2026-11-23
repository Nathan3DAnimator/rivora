// Save and load the progress (--save-test; notas-dev/salvar-progresso.md): the format's self-test first (save_progress.c:
// the checksum, damaged and newer files refused, a version 1 save migrated, the slot list), then in a new project a level
// saved to a file: a player Character (Health, Inventory), three chests (a Blueprint with a Saveable, Health, an
// Inventory, a SaveGame variable and a plain one; its On Game Loaded and On Checkpoint Reached events write variables), a
// plain Saveable cube, a Checkpoint and 1000 Saveable cubes with Health (the measure). While playing, step by step:
//   1. the world changed (moved, damaged, items given, a variable set, a chest destroyed, one spawned, the Save Game
//      object's Coins) and saved: the slot's metadata and its picture
//   2. the world changed again (another chest destroyed, the rest moved) and loaded: the level opened again and every
//      value back (place, Health, Inventory, SaveGame variable, the plain one at its default, the destroyed one gone, the
//      one destroyed after the save back, the spawned one spawned again, Coins), On Game Loaded run
//   3. the Checkpoint walked into: its save, its event; the player killed: back at the Checkpoint, alive
//   4. a timed auto save; a damaged slot refused (nothing changes); a version 1 save (the old Save Game to Slot) opened
//   5. WBP_SaveSlots: its rows and picture, a New Save from it, Back; the pause menu's Save Game / Load Game buttons
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "game_settings.h"
#include "health.h"
#include "inventory.h"
#include "level.h"
#include "online.h"
#include "platform.h"
#include "platform_files.h"
#include "project.h"
#include "save_progress.h"
#include "ui_widget.h"

#define CUBES 1000
static const float Y = 4000.0f; // (away from the template's shapes)

static int g_state, g_wait = 20;
static bool g_ok = true, g_done;
static double g_t0, g_state_t;
static uint32_t g_gem, g_loads;
static int g_fails0;
static double g_save_ms, g_build_ms, g_write_ms, g_load_ms, g_apply_ms, g_open_ms, g_thumb_ms;
static uint32_t g_bytes, g_actors;

#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: save: " __VA_ARGS__); printf("\n"); g_ok = false; } } while (0)

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] FAIL: save: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_ok = false;
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin(g, a, 1, ap), b, (uint8_t)pin(g, b, 0, bpn))) printf("[RealEngine] FAIL: save: link %s -> %s refused\n", ap, bpn), g_ok = false;
}
static uint32_t node(BpAsset* a, uint16_t kind, float x, float y, const char* text) {
    BpNode* n = bp_add_node(a, &a->graphs[0], kind, x, y, text, NULL);
    if (!n) g_ok = false;
    return n ? n->id : 0;
}

static Entity by_name(World* w, const char* name, int* count) {
    Entity first = ENTITY_NONE;
    int n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_NAME)) || (a->mask & COMP_BIT(COMP_ATTACH))) continue;
        const CName* nm = ecs_arch_column(a, COMP_NAME);
        for (uint32_t i = 0; i < a->count; i++)
            if (!strcmp(nm[i].text, name)) {
                if (!first) first = ecs_arch_entities(a)[i];
                n++;
            }
    }
    if (count) *count = n;
    return first;
}
static int saveables(World* w) {
    int n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++)
        if (w->archetypes[ai].mask & COMP_BIT(COMP_SAVEABLE)) n += (int)w->archetypes[ai].count;
    return n;
}
static void place(World* w, Entity e, float x, float y, float z) {
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t) t->location[0] = x, t->location[1] = y, t->location[2] = z, ecs_touch(w, e);
}
static bool near(World* w, Entity e, float x, float y, float z, float tol) {
    const CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
    if (!t) return false;
    float d = sqrtf((t->location[0] - x) * (t->location[0] - x) + (t->location[1] - y) * (t->location[1] - y));
    return d <= tol && fabsf(t->location[2] - z) <= tol * 2.0f;
}
static float num(World* w, Entity e, const char* var) {
    BpValue v[4];
    uint32_t wd = 0;
    bool txt = false;
    char t[8];
    return e && bp_get_instance_var(w, e, var, v, 4, &wd, t, sizeof t, &txt) ? v[0].v[0] : -1.0f;
}
static void set_num(World* w, Entity e, const char* var, float x) {
    BpValue v = { { x, 0, 0, 0 } };
    if (!e || !bp_set_instance_var(w, e, var, &v, 1)) printf("[RealEngine] FAIL: save: %s could not be set\n", var), g_ok = false;
}
static const float* pos(World* w, Entity e) {
    static const float none[3] = { -1e9f, -1e9f, -1e9f };
    const CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
    return t ? t->location : none;
}

static bool timeout(double s, const char* what) {
    if (re_time_seconds() - g_state_t < s) return false;
    printf("[RealEngine] FAIL: save: timed out waiting for %s (step %d)\n", what, g_state);
    g_ok = false, g_done = true;
    return true;
}
static void next(int s) { g_state = s, g_state_t = re_time_seconds(); }

static void step(Editor* ed, double dt) {
    World* w = &ed->world;
    (void)dt;
    if (g_done || g_wait-- > 0) return;
    Entity pl = bp_player(w);
    Entity ca = by_name(w, "ChestA", NULL), cb = by_name(w, "ChestB", NULL), cc = by_name(w, "ChestC", NULL), box = by_name(w, "PlainBox", NULL);
    switch (g_state) {
    case 0: { // 1. the world changed, then saved
        CHECK(pl && ecs_get(w, pl, COMP_HEALTH) && ca && cb && cc && box, "the level's actors are missing (player %u)", (unsigned)pl);
        if (!g_ok) {
            g_done = true;
            return;
        }
        CHECK(saveables(w) >= CUBES + 4, "%d Saveables (%d at least)", saveables(w), CUBES + 4);
        place(w, ca, 500, Y, 50), place(w, box, -300, Y + 400, 50), place(w, pl, -800, Y, 100);
        health_apply_damage(w, ca, 30.0f, "", ENTITY_NONE, ENTITY_NONE);
        health_apply_damage(w, pl, 25.0f, "", ENTITY_NONE, ENTITY_NONE);
        CHECK(inv_add(w, ca, g_gem, 3) == 3 && inv_add(w, pl, g_gem, 2) == 2, "the gems were not given");
        set_num(w, ca, "Opened", 1.0f), set_num(w, ca, "Temp", 5.0f);
        bp_destroy_actor(w, cb, true); // (an enemy killed: gone for good)
        Entity sg = progress_save_game(w);
        CHECK(sg, "no Progress Save Game (Project Settings > Save Game Class)");
        set_num(w, sg, "Coins", 42.0f);
        BpAsset* chest = bp_find_name(BPA_CLASS, "BP_SaveChest");
        Entity sp = chest ? bp_spawn(w, chest, (float[3]){ 900, Y + 300, 50 }, (float[3]){ 0, 45, 0 }) : ENTITY_NONE;
        CHECK(sp, "a chest could not be spawned");
        set_num(w, sp, "Opened", 1.0f);
        CHECK(progress_save(w, "TestSlot", "Test Save", 0), "Save Progress failed");
        g_actors = progress_stats()->actors;
        next(1);
        break;
    }
    case 1: { // the file written (after its picture's frame): its metadata
        SaveSlotInfo si;
        CHECK(progress_slot_info("TestSlot", &si), "the slot was not written at once");
        if (g_save_ms == 0.0) g_save_ms = progress_stats()->save_ms, g_build_ms = progress_stats()->build_ms, g_write_ms = progress_stats()->write_ms;
        if (!si.thumb_w && re_time_seconds() - g_state_t < 3.0) break; // (its picture: from the next frame, the file written again)
        CHECK(si.kind == SAVE_KIND_PROGRESS && !strcmp(si.level, "L_SaveTest") && !strcmp(si.display, "Test Save") && si.actors == g_actors,
              "its metadata: kind %u, level \"%s\", name \"%s\", %u actors", si.kind, si.level, si.display, si.actors);
        CHECK(si.thumb_w == SAVE_THUMB_W && si.thumb_h == SAVE_THUMB_H, "no picture in the save (%ux%u)", si.thumb_w, si.thumb_h);
        uint32_t tw = 0, th = 0, gen = 0;
        const uint8_t* px = progress_thumb("TestSlot", &tw, &th, &gen);
        long sum = 0;
        for (uint32_t i = 0; px && i < tw * th * 3; i += 97) sum += px[i];
        CHECK(px && tw == SAVE_THUMB_W && sum > 0, "the picture could not be read back (or it is black)");
        g_bytes = si.bytes, g_thumb_ms = progress_stats()->thumb_ms;
        char latest[SAVE_SLOT_NAME];
        CHECK(progress_latest(latest, sizeof latest) && !strcmp(latest, "TestSlot"), "Get Latest Save Slot: \"%s\"", latest);
        // 2. changed again after the save, then loaded
        place(w, ca, -2000, Y - 800, 50), place(w, box, 0, Y - 900, 50), place(w, pl, 1500, Y - 500, 100);
        health_heal(w, ca, 100.0f, ENTITY_NONE);
        inv_remove(w, ca, g_gem, 3);
        set_num(w, ca, "Opened", 0.0f), set_num(w, ca, "Temp", 9.0f);
        set_num(w, progress_save_game(w), "Coins", 7.0f);
        bp_destroy_actor(w, cc, true); // (destroyed after the save: the load brings it back)
        int nsp = 0;
        Entity sp = by_name(w, "BP_SaveChest", &nsp);
        if (sp) bp_destroy_actor(w, sp, true);
        g_loads = progress_stats()->loads;
        g_t0 = re_time_seconds();
        CHECK(progress_load(w, "TestSlot"), "Load Progress failed");
        CHECK(level_open_pending() != NULL, "Load Progress did not open the level");
        next(2);
        break;
    }
    case 2: { // the level opened again, the state put back
        if (progress_stats()->loads == g_loads) {
            timeout(8.0, "the load");
            break;
        }
        g_open_ms = (re_time_seconds() - g_t0) * 1000.0;
        g_load_ms = progress_stats()->load_ms, g_apply_ms = progress_stats()->apply_ms;
        CHECK(near(w, ca, 500, Y, 50, 2.0f), "ChestA's place: %.0f %.0f %.0f (500 %.0f 50)", pos(w, ca)[0], pos(w, ca)[1], pos(w, ca)[2], Y);
        CHECK(near(w, box, -300, Y + 400, 50, 2.0f), "the plain cube's place: %.0f %.0f", pos(w, box)[0], pos(w, box)[1]);
        CHECK(fabsf(health_get(w, ca) - 70.0f) < 0.01f, "ChestA's health: %.1f (70)", health_get(w, ca));
        CHECK(inv_count(w, ca, g_gem) == 3, "ChestA's gems: %d (3)", inv_count(w, ca, g_gem));
        CHECK(num(w, ca, "Opened") == 1.0f, "the SaveGame variable Opened: %.0f (1)", num(w, ca, "Opened"));
        CHECK(num(w, ca, "Temp") == 0.0f, "the plain variable Temp was saved: %.0f (its default 0)", num(w, ca, "Temp"));
        CHECK(num(w, ca, "LoadedEvent") == 1.0f, "On Game Loaded did not run (%.0f)", num(w, ca, "LoadedEvent"));
        CHECK(!cb, "ChestB (destroyed before the save) is back");
        CHECK(cc, "ChestC (destroyed after the save) did not come back");
        int nsp = 0;
        Entity sp = by_name(w, "BP_SaveChest", &nsp);
        CHECK(nsp == 1 && near(w, sp, 900, Y + 300, 50, 2.0f) && num(w, sp, "Opened") == 1.0f, "the spawned chest: %d, at %.0f %.0f", nsp, pos(w, sp)[0], pos(w, sp)[1]);
        CHECK(pl && near(w, pl, -800, Y, 100, 40.0f), "the player's place: %.0f %.0f %.0f (-800 %.0f 100)", pos(w, pl)[0], pos(w, pl)[1], pos(w, pl)[2], Y);
        CHECK(fabsf(health_get(w, pl) - 75.0f) < 0.01f && inv_count(w, pl, g_gem) == 2, "the player's health %.1f (75), gems %d (2)", health_get(w, pl), inv_count(w, pl, g_gem));
        CHECK(num(w, progress_save_game(w), "Coins") == 42.0f, "the Save Game object's Coins: %.0f (42)", num(w, progress_save_game(w), "Coins"));
        CHECK(saveables(w) == CUBES + 4, "%d Saveables after the load (%d)", saveables(w), CUBES + 4);
        CHECK(!strcmp(progress_current_slot(), "TestSlot"), "Get Current Save Slot: \"%s\"", progress_current_slot());
        // 3. the Checkpoint walked into
        place(w, pl, 2000, Y, 95);
        next(3);
        break;
    }
    case 3: {
        Entity cp = progress_checkpoint(w);
        if (!cp || !progress_slot_exists("TestCheckpoint")) {
            timeout(5.0, "the Checkpoint");
            break;
        }
        progress_flush();
        SaveSlotInfo si;
        CHECK(progress_slot_info("TestCheckpoint", &si) && (si.flags & SAVE_FLAG_CHECKPOINT), "the Checkpoint's save");
        CHECK(num(w, ca, "Checkpoints") == 1.0f, "On Checkpoint Reached ran %.0f times (1)", num(w, ca, "Checkpoints"));
        place(w, pl, -1200, Y + 600, 100);
        health_apply_damage(w, pl, 1000.0f, "", ENTITY_NONE, ENTITY_NONE); // (dead: Respawn on Death, after its 0.3 s)
        CHECK(health_is_dead(w, pl), "the player did not die");
        g_loads = progress_stats()->loads;
        next(4);
        break;
    }
    case 4: { // back at the Checkpoint (its save reloaded), alive
        if (progress_stats()->loads == g_loads) {
            timeout(6.0, "the respawn");
            break;
        }
        CHECK(pl && !health_is_dead(w, pl) && near(w, pl, 2000, Y, 95, 60.0f), "the respawn: dead %d, at %.0f %.0f %.0f (2000 %.0f)", pl ? health_is_dead(w, pl) : -1,
              pos(w, pl)[0], pos(w, pl)[1], pos(w, pl)[2], Y);
        CHECK(num(w, ca, "Checkpoints") == 0.0f, "the reached Checkpoint (Once) saved again after the load");
        // 4. a timed auto save
        progress_set_autosave_interval(0.4f);
        next(5);
        break;
    }
    case 5: {
        if (!progress_slot_exists("AutoSave_1")) {
            timeout(4.0, "the auto save");
            break;
        }
        progress_set_autosave_interval(0.0f);
        progress_flush();
        SaveSlotInfo si;
        CHECK(progress_slot_info("AutoSave_1", &si) && (si.flags & SAVE_FLAG_AUTO), "the auto save's slot");
        // a damaged slot: refused, the world as it is
        char from[700], to[700];
        progress_slot_path("TestSlot", from, sizeof from), progress_slot_path("Corrupt", to, sizeof to);
        re_copy_file(from, to);
        FILE* f = re_fopen(to, "r+b");
        if (f) fseek(f, 300, SEEK_SET), fputc(0x7E, f), fclose(f);
        uint32_t refused = progress_stats()->refused;
        CHECK(!progress_load(w, "Corrupt") && level_open_pending() == NULL && progress_stats()->refused == refused + 1, "a damaged slot was not refused");
        CHECK(!progress_slot_info("Corrupt", NULL), "Get Save Slot Info read a damaged slot");
        // a version 1 save (the old Save Game to Slot: "RESAV001" and the variables) opened by Load Game from Slot
        set_num(w, ca, "Opened", 1.0f), set_num(w, ca, "Temp", 5.0f);
        static uint8_t v1[16 * 1024];
        uint32_t n = bp_vars_dump(w, ca, v1, sizeof v1);
        char old[700];
        progress_slot_path("OldSave", old, sizeof old);
        f = re_fopen(old, "wb");
        if (f) fwrite(v1, 1, n, f), fclose(f);
        set_num(w, ca, "Opened", 0.0f), set_num(w, ca, "Temp", 99.0f);
        CHECK(n > 8 && progress_slot_info("OldSave", &si) && si.version == 1, "the version 1 file was not recognized");
        CHECK(online_load(w, ca, "OldSave") && num(w, ca, "Opened") == 1.0f && num(w, ca, "Temp") == 5.0f, "the version 1 save did not load (Opened %.0f, Temp %.0f)",
              num(w, ca, "Opened"), num(w, ca, "Temp"));
        // and Save Game to Slot's own (version 2 now, a checksum): an actor's variables and Inventory
        CHECK(online_save(w, ca, "ChestVars"), "Save Game to Slot failed");
        set_num(w, ca, "Temp", 1.0f);
        inv_remove(w, ca, g_gem, 3);
        CHECK(online_load(w, ca, "ChestVars") && num(w, ca, "Temp") == 5.0f && inv_count(w, ca, g_gem) == 3, "Load Game from Slot (version 2)");
        // 5. WBP_SaveSlots
        CHECK(save_menu_open(w, false), "WBP_SaveSlots did not open");
        next(6);
        break;
    }
    case 6: {
        if (re_time_seconds() - g_state_t < 0.2) break;
        const char* name0 = ui_test_text("Name0");
        CHECK(ui_test_shown("Row0") && ui_test_shown("ContinueButton") && !ui_test_shown("NewSaveButton"), "WBP_SaveSlots' rows (load mode)");
        CHECK(name0 && name0[0], "the first row's name is empty");
        uint32_t tw, th, gen;
        char key[80];
        CHECK(save_menu_thumb("Save.0.Thumb", key, sizeof key, &tw, &th, &gen) != NULL, "the first row has no picture");
        save_menu_close(w);
        CHECK(save_menu_open(w, true), "WBP_SaveSlots (save mode) did not open");
        next(7);
        break;
    }
    case 7: {
        if (re_time_seconds() - g_state_t < 0.2) break;
        CHECK(ui_test_shown("NewSaveButton") && !ui_test_shown("ContinueButton"), "WBP_SaveSlots' save mode");
        CHECK(ui_test_click(w, "NewSaveButton"), "New Save could not be clicked");
        next(8);
        break;
    }
    case 8: {
        if (!progress_slot_exists("Save_1")) {
            timeout(4.0, "the menu's New Save");
            break;
        }
        CHECK(ui_test_click(w, "BackButton") && !save_menu_is_open(), "Back did not close WBP_SaveSlots");
        pause_menu_assets();
        BpAsset* pm = bp_load("/Content/UI/WBP_PauseMenu.rbp");
        int sb = pm && pm->ui ? ui_tree_find(pm->ui, "SaveButton") : -1, lb = pm && pm->ui ? ui_tree_find(pm->ui, "LoadGameButton") : -1;
        int ob = pm && pm->ui ? ui_tree_find(pm->ui, "OptionsButton") : -1;
        CHECK(sb > ob && lb > sb && ob >= 0, "the pause menu's Save Game / Load Game buttons (%d %d after %d)", sb, lb, ob);
        g_done = true;
        break;
    }
    }
}

bool save_test_setup(Editor* ed) {
    World* w = &ed->world;
    g_fails0 = progress_selftest();
    if (g_fails0) printf("[RealEngine] FAIL: save: the format's self-test: %d failures\n", g_fails0), g_ok = false;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/SaveTest", ui_content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    // the gem, the Save Game class, the chest
    BpAsset* gem = bp_create(BPA_DATA, "/Content/SaveTest", "DA_SaveGem", "Item Definition");
    BpAsset* sg = bp_create(BPA_CLASS, "/Content/SaveTest", "SG_TestProgress", "Save Game");
    BpAsset* c = bp_create(BPA_CLASS, "/Content/SaveTest", "BP_SaveChest", "Actor");
    if (!gem || !sg || !c) return printf("[RealEngine] FAIL: save: could not create the assets in %s\n", dir), false;
    int k = bp_var_index(gem, "Max Stack");
    if (k >= 0) gem->vars[k].def.v[0] = 10.0f;
    bp_save(gem);
    inv_items_refresh();
    const ItemDef* gd = inv_item_by_name("DA_SaveGem");
    if (!gd) return printf("[RealEngine] FAIL: save: the gem's Item Definition\n"), false;
    g_gem = gd->hash;
    bp_add_var(sg, "Coins", BPT_INT);
    if (!bp_save(sg) || !bp_compile(sg)) return printf("[RealEngine] FAIL: save: SG_TestProgress does not compile: %s\n", sg->error), false;
    snprintf(project()->save_game_class, sizeof project()->save_game_class, "SG_TestProgress");
    c->comp_mask |= COMP_BIT(COMP_SAVEABLE) | COMP_BIT(COMP_HEALTH) | COMP_BIT(COMP_INVENTORY) | COMP_BIT(COMP_STATIC_MESH);
    ecs_component_defaults(COMP_SAVEABLE, c->comp_data[COMP_SAVEABLE]);
    ecs_component_defaults(COMP_HEALTH, c->comp_data[COMP_HEALTH]);
    ecs_component_defaults(COMP_INVENTORY, c->comp_data[COMP_INVENTORY]);
    ((CInventory*)c->comp_data[COMP_INVENTORY])->slots = 4;
    int vi = bp_add_var(c, "Opened", BPT_BOOL);
    if (vi >= 0) c->vars[vi].save_game = true;
    bp_add_var(c, "Temp", BPT_INT), bp_add_var(c, "LoadedEvent", BPT_BOOL), bp_add_var(c, "Checkpoints", BPT_INT);
    BpGraph* g = &c->graphs[0];
    uint32_t ld = node(c, BPK_EVENT_GAME_LOADED, 0, 300, NULL), set_ld = node(c, BPK_VAR_SET, 300, 300, "LoadedEvent");
    BpNode* sn = bp_node(g, set_ld);
    if (sn) sn->pins[pin(g, set_ld, 0, "LoadedEvent")].lit.v[0] = 1.0f;
    link(g, ld, "", set_ld, "");
    uint32_t ck = node(c, BPK_EVENT_CHECKPOINT, 0, 500, NULL), get_n = node(c, BPK_VAR_GET, 150, 550, "Checkpoints");
    uint32_t inc = node(c, BPK_IADD, 250, 550, NULL), set_n = node(c, BPK_VAR_SET, 400, 500, "Checkpoints");
    BpNode* incn = bp_node(g, inc);
    if (incn) incn->pins[1].lit.v[0] = 1.0f;
    link(g, ck, "", set_n, ""), link(g, get_n, "Checkpoints", inc, incn ? incn->pins[0].name : "A"), link(g, inc, incn ? incn->pins[2].name : "", set_n, "Checkpoints");
    if (!bp_save(c) || !bp_compile(c)) return printf("[RealEngine] FAIL: save: BP_SaveChest does not compile: %s\n", c->error), false;
    // the template's pawns out (the player is ours); its floor and lights stay
    for (int pass = 0; pass < 64; pass++) {
        Entity gone = ENTITY_NONE;
        for (uint32_t ai = 0; ai < w->archetype_count && !gone; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (a->count && (a->mask & (COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_BLUEPRINT))) && !(a->mask & COMP_BIT(COMP_ATTACH))) gone = ecs_arch_entities(a)[0];
        }
        if (!gone) break;
        ecs_destroy(w, gone);
    }
    project()->default_pawn[0] = 0;
    const char* names[3] = { "ChestA", "ChestB", "ChestC" };
    for (int i = 0; i < 3; i++) {
        Entity e = bp_spawn(w, c, (float[3]){ (float)i * 250.0f, Y + 200.0f, 50 }, (float[3]){ 0, 0, 0 });
        if (!e) return printf("[RealEngine] FAIL: save: a chest\n"), false;
        snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", names[i]);
        ((CTransform*)ecs_get(w, e, COMP_TRANSFORM))->scale[0] = 0.5f;
        ((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->mesh = SHAPE_CUBE;
    }
    Entity box = editor_spawn_shape(ed, SHAPE_CUBE, (float[3]){ -300, Y - 200, 50 }, 0.0);
    if (!box) return false;
    snprintf(((CName*)ecs_get(w, box, COMP_NAME))->text, RE_NAME_MAX, "PlainBox");
    ecs_add(w, box, COMP_SAVEABLE);
    ((CSaveable*)ecs_get(w, box, COMP_SAVEABLE))->id = progress_new_id();
    // the player: a Character with Health and an Inventory, possessed
    Entity hero = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_HEALTH) | COMP_BIT(COMP_INVENTORY));
    if (!hero) return false;
    snprintf(((CName*)ecs_get(w, hero, COMP_NAME))->text, RE_NAME_MAX, "SaveHero");
    CTransform* ht = ecs_get(w, hero, COMP_TRANSFORM);
    ht->location[0] = -600.0f, ht->location[1] = Y, ht->location[2] = 100.0f, ht->flags = MOBILITY_MOVABLE;
    ht->scale[0] = ht->scale[1] = ht->scale[2] = 1.0f;
    CCharacter* cc = ecs_get(w, hero, COMP_CHARACTER);
    cc->max_walk_speed = 300.0f, cc->gravity_scale = 1.0f, cc->radius = 34.0f, cc->half_height = 88.0f, cc->auto_possess = 1;
    ecs_component_defaults(COMP_HEALTH, ecs_get(w, hero, COMP_HEALTH));
    CInventory* hi = ecs_get(w, hero, COMP_INVENTORY);
    ecs_component_defaults(COMP_INVENTORY, hi);
    hi->slots = 6;
    // the Checkpoint (saves once into its slot; the player back 0.3 s after dying)
    Entity cpe = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHECKPOINT));
    if (!cpe) return false;
    snprintf(((CName*)ecs_get(w, cpe, COMP_NAME))->text, RE_NAME_MAX, "TestCheckpoint");
    CTransform* ct = ecs_get(w, cpe, COMP_TRANSFORM);
    ct->location[0] = 2000.0f, ct->location[1] = Y, ct->location[2] = 100.0f, ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f;
    CCheckpoint* cpc = ecs_get(w, cpe, COMP_CHECKPOINT);
    ecs_component_defaults(COMP_CHECKPOINT, cpc);
    cpc->respawn_delay = 0.3f, cpc->spawn_offset[2] = -90.0f;
    snprintf(cpc->slot, sizeof cpc->slot, "TestCheckpoint");
    // the floor, 1000 Saveable cubes with Health (the measure)
    Entity floor = editor_spawn_shape(ed, SHAPE_PLANE, (float[3]){ 0, Y, 0 }, 0.0);
    CTransform* ft = floor ? ecs_get(w, floor, COMP_TRANSFORM) : NULL;
    if (ft) ft->scale[0] = 60.0f, ft->scale[1] = 40.0f, ft->location[2] = -1.0f, ft->flags = MOBILITY_STATIC;
    for (int i = 0; i < CUBES; i++) {
        Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SAVEABLE) | COMP_BIT(COMP_HEALTH)); // (data: the measure, not the picture)
        if (!e) return false;
        snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "SaveCube_%d", i);
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        t->location[0] = -2500.0f + (float)(i % 50) * 100.0f, t->location[1] = Y + 1500.0f + (float)(i / 50) * 100.0f, t->location[2] = 20.0f;
        t->scale[0] = t->scale[1] = t->scale[2] = 0.3f, t->flags = MOBILITY_MOVABLE | TRANSFORM_NO_COLLISION;
        ecs_component_defaults(COMP_SAVEABLE, ecs_get(w, e, COMP_SAVEABLE));
        ecs_component_defaults(COMP_HEALTH, ecs_get(w, e, COMP_HEALTH));
    }
    // the level into a file (Load Progress opens it again)
    char lp[1100];
    snprintf(lp, sizeof lp, "%s/L_SaveTest.rlevel", dir);
    if (!editor_save_level(ed, lp, 0.0)) return printf("[RealEngine] FAIL: save: the level could not be saved in %s\n", lp), false;
    ed->cam.pos[0] = 0.0f, ed->cam.pos[1] = Y - 1800.0f, ed->cam.pos[2] = 700.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -20.0f;
    gus()->auto_save = true; // (the player's AutoSave option: on, whatever this PC's settings say)
    ed->test_step = step;
    g_state_t = re_time_seconds();
    editor_play(ed, 0.0);
    return true;
}

bool save_test_done(void) { return g_done; }

bool save_test_check(Editor* ed) {
    if (!g_done) printf("[RealEngine] FAIL: save: stopped at step %d\n", g_state), g_ok = false;
    const ProgressStats* st = progress_stats();
    printf("[RealEngine] Save cost (%u actors, %d Saveables with Health): save %.3f ms (the world %.3f ms, the file %.3f ms), load %.3f ms (apply %.3f ms; the level opened and put back in %.1f ms), "
           "file %u KB (written on a thread in %.2f ms), picture %.2f ms; progress_update %.4f ms; %u saves, %u loads, %u refused\n",
           g_actors, CUBES, g_save_ms, g_build_ms, g_write_ms, g_load_ms, g_apply_ms, g_open_ms, g_bytes / 1024u, st->disk_ms, g_thumb_ms, st->update_ms, st->saves, st->loads, st->refused);
    CHECK(g_save_ms > 0.0 && g_save_ms < 10.0, "saving 1000 Saveables costs %.3f ms (10 at most)", g_save_ms);
    CHECK(g_apply_ms > 0.0 && g_apply_ms < 20.0, "putting 1000 Saveables back costs %.3f ms (20 at most)", g_apply_ms);
    CHECK(st->update_ms < 0.2, "progress_update costs %.4f ms a frame (0.2 at most)", st->update_ms);
    editor_stop(ed, 0.0);
    g_done = true;
    const char* slots[] = { "TestSlot", "TestCheckpoint", "AutoSave_1", "AutoSave_2", "AutoSave_3", "Corrupt", "OldSave", "ChestVars", "Save_1" };
    for (size_t i = 0; i < sizeof slots / sizeof slots[0]; i++) progress_slot_delete(slots[i]);
    if (g_ok) printf("[RealEngine] Save test: OK\n");
    return g_ok;
}
