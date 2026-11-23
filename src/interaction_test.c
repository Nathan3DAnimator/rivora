// Interaction played (--interaction-test, the test interaction; notas-dev/interacao.md): first interaction_selftest, then
// in a new project, away from the template's level (y 4000), a Character (first person, the player) with an Inventory
// and around it:
//   * +X: a Door (the ready-made actor); +Y: a Door locked by Item.Key.Red (the key spent); 45 degrees: BP_Valve, a
//     Blueprint with an Interactable of 1 s hold in its class defaults, its events On Interact / On Focus Begin / On Hold
//     Progress counted into variables; 135 degrees: an Interactable behind a block (never focused: the sight);
//     -X (looking down): a Lever whose target is a gate (a Door that can't be used itself); -Y: a Chest with 2 gems
//   * 200 more Interactables in the level, the frame's interaction timed.
// Step by step: the player looks and the focus follows, WBP_InteractPrompt shows the prompt and the key (E, then X with a
// gamepad), the door opens, the locked door refuses without the key and opens with it, the valve is held 1 s, the lever
// opens the gate, the chest gives its gems.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "i18n.h"
#include "interaction.h"
#include "inventory.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_widget.h"

static Entity g_player, g_door, g_locked, g_valve, g_hidden, g_lever, g_gate, g_chest;
static int g_state, g_wait = 10;
static double g_until, g_hold0;
static bool g_ok = true, g_done;
static uint32_t g_key, g_gem;
static double g_tick_ms;
static int g_tick_n;
static float g_gate_y0;

static const float Y = 4000.0f; // (away from the template's level)

#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: interaction: " __VA_ARGS__); printf("\n"); g_ok = false; } } while (0)

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] FAIL: interaction: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_ok = false;
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin(g, a, 1, ap), b, (uint8_t)pin(g, b, 0, bpn))) printf("[RealEngine] FAIL: interaction: link %s -> %s refused\n", ap, bpn), g_ok = false;
}
static uint32_t node(BpAsset* a, uint16_t kind, float x, float y, const char* text) {
    BpNode* n = bp_add_node(a, &a->graphs[0], kind, x, y, text, NULL);
    if (!n) g_ok = false;
    return n ? n->id : 0;
}
// on event ev: Var = Var + 1
static void count_into(BpAsset* a, uint32_t ev, float y, const char* var) {
    BpGraph* g = &a->graphs[0];
    uint32_t get = node(a, BPK_VAR_GET, 300, y + 60, var), add = node(a, BPK_IADD, 500, y + 60, NULL), set = node(a, BPK_VAR_SET, 700, y, var);
    BpNode* an = bp_node(g, add);
    if (an) an->pins[1].lit.v[0] = 1.0f;
    link(g, ev, "", set, ""), link(g, get, var, add, an ? an->pins[0].name : "A"), link(g, add, an ? an->pins[2].name : "", set, var);
}
static float var_num(World* w, Entity e, const char* name) {
    BpValue v;
    return bp_instance_var(w, e, name, &v) ? v.v[0] : -12345.0f;
}

static BpAsset* item(const char* name, const char* display, const char* tags) {
    BpAsset* a = bp_create(BPA_DATA, "/Content/IATest", name, "Item Definition");
    if (!a) return NULL;
    struct { const char* f; const char* s; } set[] = { { "Display Name", display }, { "Tags", tags }, { "World Mesh", "Cube" } };
    for (size_t i = 0; i < sizeof set / sizeof set[0]; i++) {
        int k = bp_var_index(a, set[i].f);
        if (k >= 0) snprintf(a->vars[k].sdef, sizeof a->vars[k].sdef, "%s", set[i].s);
    }
    bp_save(a);
    return a;
}

static Entity device(World* w, int kind, float x, float y, float z, float yaw, const char* name) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_INTERACTABLE) | COMP_BIT(COMP_DEVICE));
    if (!e) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    ecs_component_defaults(COMP_INTERACTABLE, ecs_get(w, e, COMP_INTERACTABLE));
    ecs_component_defaults(COMP_DEVICE, ecs_get(w, e, COMP_DEVICE));
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z;
    t->rotation[1] = yaw;
    interaction_setup_device(w, e, kind);
    return e;
}

static void look(World* w, float yaw, float pitch) {
    CCharacter* c = ecs_get(w, g_player, COMP_CHARACTER);
    if (c) c->control_yaw = yaw, c->control_pitch = pitch;
}
static void wait_s(Editor* ed, double s) { g_until = ed->play_time + s; }

static void step(Editor* ed, double dt) {
    World* w = &ed->world;
    (void)dt;
    if (g_done) return;
    if (g_state >= 30) { // the measure: the frame's interaction with the 200 more Interactables in the level
        g_tick_ms += interaction_stats()->tick_ms, g_tick_n++;
        return;
    }
    if (g_wait-- > 0 || ed->play_time < g_until) return;
    if (ed->play_time > 25.0) {
        CHECK(false, "stuck at step %d", g_state);
        g_state = 30;
        return;
    }
    char text[96];
    const char* pt = ui_test_text("PromptText");
    switch (g_state) {
    case 0:
        bp_set_player(w, g_player);
        look(w, 0, 0);
        g_state = 1, g_wait = 3;
        break;
    case 1: // the door ahead: focused, the prompt on the screen with its key
        CHECK(interaction_focused(w, g_player) == g_door, "looking at +X: the door focused (%u, the door %u)", interaction_focused(w, g_player), g_door);
        CHECK(ui_test_shown("Frame") && pt && pt[0], "WBP_InteractPrompt shows the prompt (\"%s\")", pt ? pt : "(none)");
        snprintf(text, sizeof text, "%s", ui_test_text("KeyText") ? ui_test_text("KeyText") : "");
        CHECK(!strcmp(text, "E"), "the prompt's key \"%s\" (E)", text);
        CHECK(pt && !strcmp(pt, TR("Open")), "the door's prompt \"%s\" (%s)", pt ? pt : "", TR("Open"));
        bp_key_event(w, 'E', true);
        g_state = 2, g_wait = 2;
        break;
    case 2:
        bp_key_event(w, 'E', false);
        wait_s(ed, 0.9);
        g_state = 3;
        break;
    case 3: { // opened 95 degrees about its hinge; away from the player (it stood at -X: the door swings toward +X)
        const CTransform* t = ecs_get(w, g_door, COMP_TRANSFORM);
        CHECK(interaction_device_active(w, g_door), "E opened the door");
        CHECK(t && fabsf(fabsf(t->rotation[1] - 90.0f) - 95.0f) < 0.5f && t->location[0] > 200.0f, "the door swung 95 degrees away (yaw %.1f, x %.1f)",
              t ? t->rotation[1] : 0.0f, t ? t->location[0] : 0.0f);
        CHECK(pt && !strcmp(pt, TR("Close")), "the open door's prompt \"%s\" (%s)", pt ? pt : "", TR("Close"));
        look(w, 90, 0);
        g_state = 4, g_wait = 3;
        break;
    }
    case 4: // the locked door: focused, it says what it needs; E refused
        CHECK(interaction_focused(w, g_player) == g_locked, "looking at +Y: the locked door focused");
        CHECK(pt && strstr(pt, "Red Key"), "the locked door's prompt names the key: \"%s\"", pt ? pt : "(none)");
        bp_key_event(w, 'E', true);
        g_state = 5, g_wait = 2;
        break;
    case 5:
        bp_key_event(w, 'E', false);
        wait_s(ed, 0.3);
        g_state = 6;
        break;
    case 6:
        CHECK(!interaction_device_active(w, g_locked) && interaction_stats()->refused >= 1, "without the key the door stays shut");
        CHECK(inv_add(w, g_player, g_key, 1) == 1, "the key into the player's Inventory");
        g_state = 7, g_wait = 2;
        break;
    case 7:
        CHECK(pt && !strcmp(pt, TR("Open")), "with the key: \"%s\" (%s)", pt ? pt : "", TR("Open"));
        bp_key_event(w, 'E', true);
        g_state = 8, g_wait = 2;
        break;
    case 8:
        bp_key_event(w, 'E', false);
        wait_s(ed, 0.9);
        g_state = 9;
        break;
    case 9:
        CHECK(interaction_device_active(w, g_locked), "with the key the door opens");
        CHECK(inv_count(w, g_player, g_key) == 0, "the key spent (Consume Item): %d left", inv_count(w, g_player, g_key));
        look(w, 45, 0);
        g_state = 10, g_wait = 3;
        break;
    case 10: // the valve (a Blueprint): On Focus Begin; held
        CHECK(interaction_focused(w, g_player) == g_valve, "looking at 45 degrees: the valve focused (%u, the valve %u)", interaction_focused(w, g_player), g_valve);
        CHECK(var_num(w, g_valve, "Focus") == 1.0f, "the valve's Event On Focus Begin (%.0f)", var_num(w, g_valve, "Focus"));
        CHECK(pt && !strcmp(pt, "Turn Valve"), "the valve's prompt \"%s\" (Turn {0} with its Name)", pt ? pt : "");
        bp_key_event(w, 'E', true);
        g_hold0 = ed->play_time;
        wait_s(ed, 0.5);
        g_state = 11;
        break;
    case 11: { // half way: nothing yet, the progress about 0.5 in the event and the bar
        float lp = var_num(w, g_valve, "LastProgress");
        const UiWidget* bar = ui_test_widget("HoldBar");
        CHECK(var_num(w, g_valve, "Uses") == 0.0f, "held %.2f s of 1: not used yet", ed->play_time - g_hold0);
        CHECK(lp > 0.3f && lp < 0.75f, "Event On Hold Progress: %.2f (about 0.5)", lp);
        CHECK(ui_test_shown("HoldBar") && bar && bar->percent > 0.3f && bar->percent < 0.75f, "the hold's bar on the screen (%.2f)", bar ? bar->percent : -1.0f);
        g_until = g_hold0 + 1.15;
        g_state = 12;
        break;
    }
    case 12:
        CHECK(var_num(w, g_valve, "Uses") == 1.0f, "held 1 s: the valve's Event On Interact once (%.0f)", var_num(w, g_valve, "Uses"));
        bp_key_event(w, 'E', false);
        look(w, 135, 0);
        g_state = 13, g_wait = 3;
        break;
    case 13: // behind the block: not focused
        CHECK(interaction_focused(w, g_player) == ENTITY_NONE, "an Interactable behind a block is not focused (%u, it %u)", interaction_focused(w, g_player), g_hidden);
        CHECK(!ui_test_shown("Frame"), "nothing in focus: the prompt hidden");
        look(w, 180, -35);
        g_state = 14, g_wait = 3;
        break;
    case 14: // the lever (looking down)
        CHECK(interaction_focused(w, g_player) == g_lever, "looking down at -X: the lever focused (%u, the lever %u)", interaction_focused(w, g_player), g_lever);
        bp_key_event(w, 'E', true);
        g_state = 15, g_wait = 2;
        break;
    case 15:
        bp_key_event(w, 'E', false);
        wait_s(ed, 0.9);
        g_state = 16;
        break;
    case 16: {
        const CTransform* gt = ecs_get(w, g_gate, COMP_TRANSFORM);
        CHECK(interaction_device_active(w, g_lever) && interaction_device_active(w, g_gate), "the lever on, its gate open");
        CHECK(gt && fabsf(gt->location[1] - g_gate_y0) > 50.0f, "the gate moved (%.1f from %.1f)", gt ? gt->location[1] : 0.0f, g_gate_y0);
        look(w, 270, -35);
        g_state = 17, g_wait = 3;
        break;
    }
    case 17: // the chest
        CHECK(interaction_focused(w, g_player) == g_chest, "looking down at -Y: the chest focused");
        bp_key_event(w, 'E', true);
        g_state = 18, g_wait = 2;
        break;
    case 18:
        bp_key_event(w, 'E', false);
        wait_s(ed, 0.3);
        g_state = 19;
        break;
    case 19:
        CHECK(interaction_device_active(w, g_chest) && inv_count(w, g_player, g_gem) == 2, "the chest opened and gave its 2 gems (%d)", inv_count(w, g_player, g_gem));
        interaction_test_gamepad(1);
        g_state = 20, g_wait = 3;
        break;
    case 20:
        snprintf(text, sizeof text, "%s", ui_test_text("KeyText") ? ui_test_text("KeyText") : "");
        CHECK(!strcmp(text, "X"), "with a gamepad the prompt's key is \"%s\" (X)", text);
        interaction_test_gamepad(-1);
        look(w, 0, 0);
        g_state = 30;
        break;
    default: g_state = 30; break;
    }
}

bool interaction_test_setup(Editor* ed) {
    if (interaction_selftest()) {
        printf("[RealEngine] FAIL: interaction: the self-test failed\n");
        return false;
    }
    World* w = &ed->world;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/IATest", ui_content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    BpAsset* key = item("DA_RedKey", "Red Key", "Item.Key.Red");
    BpAsset* gem = item("DA_Gem", "Gem", "Item.Gem");
    if (!key || !gem || !interaction_prompt_asset()) {
        printf("[RealEngine] FAIL: interaction: the assets (DA_RedKey, DA_Gem, WBP_InteractPrompt)\n");
        return false;
    }
    inv_items_refresh();
    const ItemDef *dk = inv_item_by_name("DA_RedKey"), *dg = inv_item_by_name("DA_Gem");
    if (!dk || !dg) {
        printf("[RealEngine] FAIL: interaction: the Item Definitions\n");
        return false;
    }
    g_key = dk->hash, g_gem = dg->hash;
    // BP_Valve: an Interactable in its class defaults (hold 1 s, "Turn {0}"), its events counted
    BpAsset* vc = bp_create(BPA_CLASS, "/Content/IATest", "BP_Valve", "Actor");
    if (!vc) return false;
    vc->comp_mask |= COMP_BIT(COMP_INTERACTABLE);
    CInteractable* ci = (CInteractable*)vc->comp_data[COMP_INTERACTABLE];
    memset(ci, 0, sizeof *ci);
    ecs_component_defaults(COMP_INTERACTABLE, ci);
    ci->hold_time = 1.0f;
    snprintf(ci->prompt, sizeof ci->prompt, "Turn {0}"), snprintf(ci->name, sizeof ci->name, "Valve");
    bp_add_var(vc, "Uses", BPT_INT), bp_add_var(vc, "Focus", BPT_INT), bp_add_var(vc, "LastProgress", BPT_FLOAT);
    BpGraph* g = &vc->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    count_into(vc, node(vc, BPK_EVENT_IA_INTERACT, 0, 0, NULL), 0, "Uses");
    count_into(vc, node(vc, BPK_EVENT_IA_FOCUS_BEGIN, 0, 250, NULL), 250, "Focus");
    uint32_t hp = node(vc, BPK_EVENT_IA_HOLD, 0, 500, NULL), sp = node(vc, BPK_VAR_SET, 300, 500, "LastProgress");
    link(g, hp, "", sp, ""), link(g, hp, "Progress", sp, "LastProgress");
    if (!g_ok || !bp_save(vc) || !bp_compile(vc)) {
        printf("[RealEngine] FAIL: interaction: BP_Valve does not compile: %s\n", vc->error);
        return false;
    }
    // the floor, the player (first person, no gravity: it stays where the test aims from), the actors around it
    Entity floor = editor_spawn_shape(ed, SHAPE_PLANE, (float[3]){ 0, Y, 0 }, 0.0);
    CTransform* ft = floor ? ecs_get(w, floor, COMP_TRANSFORM) : NULL;
    if (ft) ft->scale[0] = 40.0f, ft->scale[1] = 40.0f, ft->location[2] = -1.0f, ft->flags = MOBILITY_STATIC;
    g_player = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_INVENTORY));
    if (!g_player) return false;
    CTransform* pt = ecs_get(w, g_player, COMP_TRANSFORM);
    pt->location[0] = 0, pt->location[1] = Y, pt->location[2] = 100.0f, pt->flags = MOBILITY_MOVABLE;
    CCharacter* cc = ecs_get(w, g_player, COMP_CHARACTER);
    cc->max_walk_speed = 300.0f, cc->gravity_scale = 0.0f, cc->radius = 34.0f, cc->half_height = 88.0f, cc->camera_distance = 0.0f, cc->auto_possess = 1;
    ecs_component_defaults(COMP_INVENTORY, ecs_get(w, g_player, COMP_INVENTORY));
    ((CInventory*)ecs_get(w, g_player, COMP_INVENTORY))->interact_key = 0; // (its Pickups' key off: the Interactor's alone)
    snprintf(((CName*)ecs_get(w, g_player, COMP_NAME))->text, RE_NAME_MAX, "IAPlayer");
    g_door = device(w, DEV_DOOR, 200, Y, 100, 90, "Door");
    g_locked = device(w, DEV_DOOR, 0, Y + 200, 100, 0, "LockedDoor");
    CInteractable* lc = g_locked ? ecs_get(w, g_locked, COMP_INTERACTABLE) : NULL;
    if (lc) snprintf(lc->require, sizeof lc->require, "Item.Key.Red"), lc->flags |= IA_CONSUME;
    g_valve = bp_spawn(w, vc, (float[3]){ 140, Y + 140, 166 }, (float[3]){ 0, 0, 0 });
    CTransform* vt = g_valve ? ecs_get(w, g_valve, COMP_TRANSFORM) : NULL;
    if (vt) vt->scale[0] = vt->scale[1] = vt->scale[2] = 0.4f;
    Entity block = editor_spawn_shape(ed, SHAPE_CUBE, (float[3]){ -90, Y + 90, 150 }, 0.0);
    CTransform* bt = block ? ecs_get(w, block, COMP_TRANSFORM) : NULL;
    if (bt) bt->scale[0] = bt->scale[1] = 0.8f, bt->scale[2] = 3.0f, bt->flags = MOBILITY_STATIC;
    g_hidden = device(w, DEV_BUTTON, -160, Y + 160, 166, 0, "HiddenButton");
    g_lever = device(w, DEV_LEVER, -150, Y, 0, 0, "Lever");
    g_gate = device(w, DEV_DOOR, -600, Y, 100, 90, "Gate");
    CInteractable* gc = g_gate ? ecs_get(w, g_gate, COMP_INTERACTABLE) : NULL;
    if (gc) gc->flags &= ~IA_ENABLED; // (opened by the lever only)
    CDevice* gd = g_gate ? ecs_get(w, g_gate, COMP_DEVICE) : NULL;
    if (gd) gd->motion = DEV_SLIDE, gd->amount = 120.0f, gd->flags = 0;
    if (g_gate) g_gate_y0 = ((CTransform*)ecs_get(w, g_gate, COMP_TRANSFORM))->location[1];
    CDevice* ld = g_lever ? ecs_get(w, g_lever, COMP_DEVICE) : NULL;
    if (ld) ld->target = g_gate;
    g_chest = device(w, DEV_CHEST, 0, Y - 170, 0, 0, "Chest");
    CDevice* cd = g_chest ? ecs_get(w, g_chest, COMP_DEVICE) : NULL;
    if (cd) cd->item[0] = g_gem, cd->count[0] = 2;
    if (!g_door || !g_locked || !g_valve || !block || !g_hidden || !g_lever || !g_gate || !g_chest) {
        printf("[RealEngine] FAIL: interaction: the actors\n");
        return false;
    }
    for (int i = 0; i < 200; i++) { // (the measure: 200 more Interactables in the level, 1.5 m apart)
        Entity e = editor_spawn_shape(ed, SHAPE_CUBE, (float[3]){ -1500.0f + (float)(i % 20) * 150.0f, Y + 1200.0f + (float)(i / 20) * 150.0f, 50 }, 0.0);
        if (!e || !ecs_add(w, e, COMP_INTERACTABLE)) continue;
        ecs_component_defaults(COMP_INTERACTABLE, ecs_get(w, e, COMP_INTERACTABLE));
        CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        if (t) t->scale[0] = t->scale[1] = t->scale[2] = 0.3f;
    }
    ed->cam.pos[0] = -200.0f, ed->cam.pos[1] = Y - 700.0f, ed->cam.pos[2] = 400.0f;
    ed->cam.yaw = 70.0f, ed->cam.pitch = -25.0f;
    ed->test_step = step;
    editor_play(ed, 0.0);
    return true;
}

bool interaction_test_check(Editor* ed) {
    if (g_state < 30) printf("[RealEngine] FAIL: interaction: stopped at step %d\n", g_state), g_ok = false;
    const InteractStats* st = interaction_stats();
    double tick = g_tick_n ? g_tick_ms / g_tick_n : 0.0;
    printf("[RealEngine] Interaction cost: %.4f ms a frame (%u Interactables, %u Interactors, %u devices; %u used, %u refused, %u focus changes)\n", tick,
           st->interactables, st->interactors, st->devices, st->interactions, st->refused, st->focus_changes);
    CHECK(g_tick_n > 0, "the measure ran no frame");
    CHECK(st->interactables >= 207, "the walk saw %u Interactables (207)", st->interactables);
    CHECK(tick < 0.1, "the interaction costs %.4f ms a frame (0.1 at most)", tick);
    editor_stop(ed, 0.0);
    g_done = true;
    if (g_ok) printf("[RealEngine] Interaction test: OK\n");
    return g_ok;
}
