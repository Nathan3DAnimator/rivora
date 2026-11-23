// Inventory and Items (--inventory-test, the test inventory; notas-dev/inventario.md): in a new project, three Item
// Definitions (a potion that stacks by 5 and is consumable, a sword for the right hand, a heavy rock), a Blueprint actor
// with an Inventory of 4 slots and 20 kg (3 rocks to start with; its BeginPlay adds 2 potions with Add Item; its
// events On Item Used / On Item Equipped / On Inventory Changed write variables) and a Character with the Mannequin.
// While playing, step by step: stacks that overflow, the weight limit, remove, use, split, move (merge and swap), sort,
// find by tag, equip (on the actor; on the Mannequin's right hand bone) and unequip, Save Game to Slot / load back,
// drop and pick back up, a Pickup that comes back, a Pickup taken with the interact key; 256 spinning Pickups measured.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "anim.h"
#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "inventory.h"
#include "online.h"
#include "platform.h"
#include "platform_files.h"
#include "ui_widget.h"

static Entity g_t, g_hero, g_drop, g_respawn, g_interact;
static int g_state, g_wait = 10; // (the first frames: the Mannequin's parts made)
static double g_hidden_at;
static bool g_ok = true, g_done;
static uint32_t g_potion, g_sword, g_rock;
static double g_tick_ms, g_op_us;
static int g_tick_n;
static uint32_t g_snap_items[4], g_snap_eq;
static int g_snap_counts[4];
static Entity g_widget;
static float g_a[2], g_b[2];
static double g_draw_ms;
static int g_draw_n;

#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: inventory: " __VA_ARGS__); printf("\n"); g_ok = false; } } while (0)

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] FAIL: inventory: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_ok = false;
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin(g, a, 1, ap), b, (uint8_t)pin(g, b, 0, bpn))) printf("[RealEngine] FAIL: inventory: link %s -> %s refused\n", ap, bpn), g_ok = false;
}
static uint32_t node(BpAsset* a, uint16_t kind, float x, float y, const char* text) {
    BpNode* n = bp_add_node(a, &a->graphs[0], kind, x, y, text, NULL);
    if (!n) g_ok = false;
    return n ? n->id : 0;
}

static BpAsset* item(const char* name, const char* display, int stack, float weight, const char* tags, const char* slot, const char* mesh) {
    BpAsset* a = bp_create(BPA_DATA, "/Content/InvTest", name, "Item Definition");
    if (!a) return NULL;
    struct { const char* f; const char* s; float v; } set[] = { { "Display Name", display, 0 }, { "Tags", tags, 0 }, { "Equip Slot", slot, 0 },
                                                                 { "World Mesh", mesh, 0 }, { "Description", "A test item.", 0 } };
    for (size_t i = 0; i < sizeof set / sizeof set[0]; i++) {
        int k = bp_var_index(a, set[i].f);
        if (k >= 0) snprintf(a->vars[k].sdef, sizeof a->vars[k].sdef, "%s", set[i].s);
        else printf("[RealEngine] FAIL: inventory: the Item Definition has no \"%s\"\n", set[i].f), g_ok = false;
    }
    int k = bp_var_index(a, "Max Stack");
    if (k >= 0) a->vars[k].def.v[0] = (float)stack;
    k = bp_var_index(a, "Weight");
    if (k >= 0) a->vars[k].def.v[0] = weight;
    bp_save(a);
    return a;
}

static Entity pickup(World* w, uint32_t it, int count, const float at[3], uint32_t mode, float respawn) {
    Entity p = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_PICKUP));
    if (!p) return p;
    CTransform* t = ecs_get(w, p, COMP_TRANSFORM);
    memcpy(t->location, at, 12);
    t->scale[0] = t->scale[1] = t->scale[2] = 0.2f, t->flags = MOBILITY_MOVABLE;
    CPickup* pk = ecs_get(w, p, COMP_PICKUP);
    pk->item = it, pk->count = (uint32_t)count, pk->mode = mode, pk->respawn_time = respawn, pk->spin = 1;
    snprintf(((CName*)ecs_get(w, p, COMP_NAME))->text, RE_NAME_MAX, "TestPickup");
    return p;
}

static float dist(World* w, Entity a, Entity b) {
    const CTransform* x = ecs_get(w, a, COMP_TRANSFORM);
    const CTransform* y = ecs_get(w, b, COMP_TRANSFORM);
    if (!x || !y) return 1e9f;
    float d[3] = { x->location[0] - y->location[0], x->location[1] - y->location[1], x->location[2] - y->location[2] };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}
static void place(World* w, Entity e, const float at[3]) {
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t) memcpy(t->location, at, 12), ecs_touch(w, e);
}
static bool var_text(World* w, Entity e, const char* name, char* out, size_t cap) {
    BpValue v[4];
    uint32_t wd = 0;
    bool is_text = false;
    out[0] = 0;
    return bp_get_instance_var(w, e, name, v, 4, &wd, out, cap, &is_text) && is_text;
}
static float var_num(World* w, Entity e, const char* name) {
    BpValue v[4];
    uint32_t wd = 0;
    bool is_text = false;
    char t[8];
    return bp_get_instance_var(w, e, name, v, 4, &wd, t, sizeof t, &is_text) ? v[0].v[0] : -1.0f;
}

static const float Y = 3000.0f; // (away from the default level's shapes and the template's player)

static void step(Editor* ed, double dt) {
    World* w = &ed->world;
    (void)dt;
    if (g_done) return;
    if (g_state >= 20) { // the measure: the frame's inventory work with 256 spinning Pickups around
        g_tick_ms += inv_stats()->tick_ms, g_tick_n++;
        g_draw_ms += ui_stats()->draw_ms, g_draw_n++;
        return;
    }
    if (g_wait-- > 0) return;
    uint32_t it = 0;
    int n = 0;
    switch (g_state) {
    case 0: {
        CHECK(inv_count(w, g_t, g_rock) == 3, "starting items: %d rocks (3)", inv_count(w, g_t, g_rock));
        CHECK(inv_count(w, g_t, g_potion) == 2, "BeginPlay's Add Item: %d potions (2)", inv_count(w, g_t, g_potion));
        // stacks that overflow: 9 potions fill the potion stack (5) and two more slots; 6 more find room for 4
        CHECK(inv_add(w, g_t, g_potion, 9) == 9, "9 potions into the stacks");
        CHECK(inv_add(w, g_t, g_potion, 6) == 4, "the slots full: 4 of 6 more potions fit");
        CHECK(inv_count(w, g_t, g_potion) == 15, "%d potions (15)", inv_count(w, g_t, g_potion));
        float mx = 0.0f, wt = inv_weight(w, g_t, &mx);
        CHECK(fabsf(wt - 19.5f) < 0.01f && mx == 20.0f, "weight %.2f / %.0f kg (19.5 / 20)", wt, mx);
        CHECK(inv_add(w, g_t, g_rock, 1) == 0, "a rock over the weight limit went in");
        // remove: from the last stacks
        CHECK(inv_remove(w, g_t, g_potion, 7) == 7, "remove 7 potions");
        CHECK(inv_count(w, g_t, g_potion) == 8, "%d potions after removing (8)", inv_count(w, g_t, g_potion));
        CHECK(!inv_get_slot(w, g_t, 3, &it, &n), "the last slot emptied first");
        // use: a consumable spends one, a rock stays
        CHECK(inv_use(w, g_t, g_potion) && inv_count(w, g_t, g_potion) == 7, "Use Item on a potion");
        CHECK(inv_use(w, g_t, g_rock) && inv_count(w, g_t, g_rock) == 3, "Use Item on a rock");
        // split, move (merge, swap), sort, find by tag
        CHECK(inv_split(w, g_t, 1, 2) && inv_get_slot(w, g_t, 3, &it, &n) && it == g_potion && n == 2, "Split Stack");
        CHECK(inv_move(w, g_t, 3, 2) && inv_get_slot(w, g_t, 2, &it, &n) && n == 5 && !inv_get_slot(w, g_t, 3, NULL, NULL), "Move Item (merge)");
        CHECK(inv_move(w, g_t, 0, 3) && inv_get_slot(w, g_t, 3, &it, &n) && it == g_rock && !inv_get_slot(w, g_t, 0, NULL, NULL), "Move Item (swap)");
        inv_sort(w, g_t);
        uint32_t s0, s1, s2;
        int c0, c1, c2;
        inv_get_slot(w, g_t, 0, &s0, &c0), inv_get_slot(w, g_t, 1, &s1, &c1), inv_get_slot(w, g_t, 2, &s2, &c2);
        CHECK(s0 == g_potion && c0 == 5 && s1 == g_potion && c1 == 2 && s2 == g_rock && c2 == 3 && !inv_get_slot(w, g_t, 3, NULL, NULL),
              "Sort Inventory (potions merged first, rocks, the empty slot last)");
        int slot = -1;
        CHECK(inv_find_tag(w, g_t, "Item.Material", &it, &slot) && it == g_rock && slot == 2, "Find Item by Tag (Item.Material)");
        CHECK(!inv_find_tag(w, g_t, "Item.Weapon", &it, &slot) && slot == -1, "Find Item by Tag: no weapon yet");
        // equip on an actor without a skeleton: on the actor itself
        CHECK(inv_add(w, g_t, g_sword, 1) == 1, "the sword in");
        CHECK(inv_equip(w, g_t, g_sword) && inv_equipped(w, g_t, INV_EQ_RIGHT_HAND) == g_sword && inv_count(w, g_t, g_sword) == 0, "Equip Item");
        CHECK(inv_equipped_entity(w, g_t, INV_EQ_RIGHT_HAND) != ENTITY_NONE, "the equipped sword's mesh");
        // Save Game to Slot: the variables and the inventory
        for (int i = 0; i < 4; i++) inv_get_slot(w, g_t, i, &g_snap_items[i], &g_snap_counts[i]);
        g_snap_eq = inv_equipped(w, g_t, INV_EQ_RIGHT_HAND);
        CHECK(online_save(w, g_t, "InvTest"), "Save Game to Slot");
        // the Character with the Mannequin: the sword on its right hand bone
        CHECK(inv_add(w, g_hero, g_sword, 1) == 1 && inv_equip(w, g_hero, g_sword), "Equip Item on the Character");
        g_state = 1, g_wait = 3;
        break;
    }
    case 1: {
        char t[64];
        CHECK(var_text(w, g_t, "LastUsed", t, sizeof t) && !strcmp(t, "DA_TestRock"), "Event On Item Used: LastUsed \"%s\" (DA_TestRock)", t);
        CHECK(var_text(w, g_t, "EquippedSlot", t, sizeof t) && !strcmp(t, "Right Hand"), "Event On Item Equipped: \"%s\" (Right Hand)", t);
        CHECK(var_num(w, g_t, "Changes") >= 1.0f, "Event On Inventory Changed did not run");
        Entity sw = inv_equipped_entity(w, g_hero, INV_EQ_RIGHT_HAND), hand = anim_bone_entity(w, g_hero, "hand_r");
        const CAttach* at = ecs_get(w, sw, COMP_ATTACH);
        CHECK(hand != ENTITY_NONE && at && at->parent == hand, "the sword hangs on the hand_r bone (parent %u, the hand %u)", at ? at->parent : 0, hand);
        float d = dist(w, sw, hand);
        CHECK(d < 30.0f, "the sword is %.1f cm from the hand", d);
        printf("[RealEngine] inventory: the sword %.1f cm from the Mannequin's hand_r; events ran\n", d);
        // unequip: back in the slots, its mesh gone
        CHECK(inv_unequip(w, g_hero, INV_EQ_RIGHT_HAND) && inv_count(w, g_hero, g_sword) == 1 && !ecs_alive(w, sw), "Unequip Item");
        // Load Game from Slot: emptied, then read back
        inv_clear(w, g_t);
        CHECK(inv_count(w, g_t, g_potion) == 0 && !inv_equipped(w, g_t, INV_EQ_RIGHT_HAND), "the inventory emptied");
        CHECK(online_load(w, g_t, "InvTest"), "Load Game from Slot");
        bool same = inv_equipped(w, g_t, INV_EQ_RIGHT_HAND) == g_snap_eq && inv_equipped_entity(w, g_t, INV_EQ_RIGHT_HAND) != ENTITY_NONE;
        for (int i = 0; i < 4; i++) {
            inv_get_slot(w, g_t, i, &it, &n);
            same &= it == g_snap_items[i] && n == g_snap_counts[i];
        }
        CHECK(same, "the loaded inventory is not the saved one");
        // drop: a Pickup in front, not taken back while the dropper stands there
        g_drop = inv_drop(w, g_t, g_rock, 1);
        CHECK(g_drop != ENTITY_NONE && inv_count(w, g_t, g_rock) == 2, "Drop Item");
        CHECK(dist(w, g_t, g_drop) > 80.0f, "the dropped Pickup is %.0f cm away", dist(w, g_t, g_drop));
        g_state = 2, g_wait = 3;
        break;
    }
    case 2:
        CHECK(ecs_alive(w, g_drop) && inv_count(w, g_t, g_rock) == 2, "the dropper took its Pickup back at once");
        place(w, g_t, (float[3]){ 600, Y, 100 }); // (away: then onto it)
        g_state = 3, g_wait = 2;
        break;
    case 3: {
        const CTransform* pt = ecs_get(w, g_drop, COMP_TRANSFORM);
        if (pt) place(w, g_t, pt->location);
        g_state = 4, g_wait = 2;
        break;
    }
    case 4: {
        CHECK(!ecs_alive(w, g_drop) && inv_count(w, g_t, g_rock) == 3, "walking onto the dropped Pickup: rocks %d (3)", inv_count(w, g_t, g_rock));
        const CTransform* pt = ecs_get(w, g_respawn, COMP_TRANSFORM);
        if (pt) place(w, g_t, pt->location);
        g_state = 5, g_wait = 2;
        break;
    }
    case 5: {
        const CTransform* pt = ecs_get(w, g_respawn, COMP_TRANSFORM);
        CHECK(ecs_alive(w, g_respawn) && pt && (pt->flags & TRANSFORM_HIDDEN_GAME), "the Pickup with a respawn time hides once taken");
        CHECK(inv_count(w, g_t, g_potion) == 9, "%d potions after the Pickup (9)", inv_count(w, g_t, g_potion));
        place(w, g_t, (float[3]){ -800, Y, 100 });
        g_hidden_at = ed->play_time;
        g_state = 6, g_wait = 2;
        break;
    }
    case 6: {
        const CTransform* pt = ecs_get(w, g_respawn, COMP_TRANSFORM);
        if (pt && (pt->flags & TRANSFORM_HIDDEN_GAME) && ed->play_time - g_hidden_at < 2.0) return; // (its 0.4 s)
        CHECK(pt && !(pt->flags & TRANSFORM_HIDDEN_GAME), "the Pickup did not come back");
        // the interact key: the possessed Character takes the On Interact Pickup in its reach
        bp_set_player(w, g_hero);
        const CTransform* ht = ecs_get(w, g_hero, COMP_TRANSFORM);
        if (ht) g_interact = pickup(w, g_potion, 1, (float[3]){ ht->location[0] + 60, ht->location[1], ht->location[2] - 60 }, PICKUP_ON_INTERACT, 0.0f);
        g_state = 7, g_wait = 2;
        break;
    }
    case 7:
        CHECK(ecs_alive(w, g_interact) && inv_count(w, g_hero, g_potion) == 0, "the On Interact Pickup was taken without the key");
        bp_key_event(w, 'E', true);
        g_state = 8, g_wait = 2;
        break;
    case 8: {
        bp_key_event(w, 'E', false);
        CHECK(!ecs_alive(w, g_interact) && inv_count(w, g_hero, g_potion) == 1, "the interact key took the Pickup (%d potions)", inv_count(w, g_hero, g_potion));
        // an operation's cost: Add + Remove of one potion, 1000 times
        double t0 = re_time_seconds();
        for (int i = 0; i < 1000; i++) inv_add(w, g_t, g_potion, 1), inv_remove(w, g_t, g_potion, 1);
        g_op_us = (re_time_seconds() - t0) * 1e6 / 1000.0;
        // WBP_Inventory on the screen: the player's (the Character's) slots and equipment
        BpAsset* wbp = bp_find_hash(inv_widget_asset());
        g_widget = wbp ? ui_create_widget(w, wbp) : ENTITY_NONE;
        CHECK(g_widget != ENTITY_NONE, "Create Widget (WBP_Inventory)");
        if (g_widget) ui_add_to_viewport(g_widget, 0), ui_show_mouse(true);
        g_state = 10, g_wait = 3;
        break;
    }
    // drag and drop with the test's mouse: slot 0 (the sword) to slot 5, then onto the equipment; a right click uses
    case 10:
        if (!inv_widget_cell(g_hero, false, 0, g_a) || !inv_widget_cell(g_hero, false, 5, g_b)) {
            CHECK(false, "WBP_Inventory's grid was not drawn");
            g_state = 20;
            break;
        }
        CHECK(ui_test_widget("Slots") && ui_test_widget("Equipment"), "WBP_Inventory's Slots / Equipment");
        inv_widget_test_input(true, g_a[0], g_a[1], true, true, false);
        g_state = 11, g_wait = 1;
        break;
    case 11: inv_widget_test_input(true, g_b[0], g_b[1], true, false, false), g_state = 12, g_wait = 1; break;
    case 12: inv_widget_test_input(true, g_b[0], g_b[1], false, false, false), g_state = 13, g_wait = 1; break;
    case 13:
        CHECK(inv_get_slot(w, g_hero, 5, &it, &n) && it == g_sword && !inv_get_slot(w, g_hero, 0, NULL, NULL), "dragged slot 0 to slot 5");
        if (!inv_widget_cell(g_hero, true, INV_EQ_RIGHT_HAND, g_a)) CHECK(false, "the equipment grid");
        inv_widget_test_input(true, g_b[0], g_b[1], true, true, false);
        g_state = 14, g_wait = 1;
        break;
    case 14: inv_widget_test_input(true, g_a[0], g_a[1], true, false, false), g_state = 15, g_wait = 1; break;
    case 15: inv_widget_test_input(true, g_a[0], g_a[1], false, false, false), g_state = 16, g_wait = 1; break;
    case 16: {
        CHECK(inv_equipped(w, g_hero, INV_EQ_RIGHT_HAND) == g_sword && inv_count(w, g_hero, g_sword) == 0, "dragged onto the Right Hand: equipped");
        float p[2];
        int ps = -1;
        CHECK(inv_find_tag(w, g_hero, "Item.Potion", &it, &ps) && inv_widget_cell(g_hero, false, ps, p), "the potion's slot");
        inv_widget_test_input(true, p[0], p[1], false, false, true);
        g_state = 17, g_wait = 1;
        break;
    }
    case 17:
        CHECK(inv_count(w, g_hero, g_potion) == 0, "a right click used the potion (%d left)", inv_count(w, g_hero, g_potion));
        inv_widget_test_input(false, 0, 0, false, false, false);
        g_state = 20;
        break;
    }
}

bool inventory_test_setup(Editor* ed) {
    World* w = &ed->world;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/InvTest", ui_content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    BpAsset* p = item("DA_TestPotion", "Test Potion", 5, 0.5f, "Item.Consumable, Item.Potion", "", "Sphere");
    BpAsset* s = item("DA_TestSword", "Test Sword", 1, 3.0f, "Item.Weapon.Sword", "Right Hand", "Cylinder");
    BpAsset* r = item("DA_TestRock", "Test Rock", 10, 4.0f, "Item.Material", "", "Cube");
    BpAsset* c = bp_create(BPA_CLASS, "/Content/InvTest", "BP_InvTester", "Actor");
    if (!p || !s || !r || !c) {
        printf("[RealEngine] FAIL: inventory: could not create the assets in %s\n", dir);
        return false;
    }
    inv_items_refresh();
    const ItemDef *dp = inv_item_by_name("DA_TestPotion"), *ds = inv_item_by_name("dA_testsword"), *dr = inv_item_by_name("Test Rock");
    if (!dp || !ds || !dr || ds->equip != INV_EQ_RIGHT_HAND || !dp->consumable || dr->consumable || dp->max_stack != 5) {
        printf("[RealEngine] FAIL: inventory: the Item Definitions read wrong\n");
        return false;
    }
    g_potion = dp->hash, g_sword = ds->hash, g_rock = dr->hash;
    // the Blueprint: its Inventory (4 slots, 20 kg, 3 rocks), BeginPlay adds 2 potions; the events write variables
    c->comp_mask |= COMP_BIT(COMP_INVENTORY);
    CInventory* ci = (CInventory*)c->comp_data[COMP_INVENTORY];
    ecs_component_defaults(COMP_INVENTORY, ci);
    ci->slots = 4, ci->max_weight = 20.0f, ci->start_item[0] = g_rock, ci->start_count[0] = 3, ci->pickup_radius = 100.0f;
    bp_add_var(c, "LastUsed", BPT_NAME), bp_add_var(c, "EquippedSlot", BPT_NAME), bp_add_var(c, "Changes", BPT_INT);
    uint32_t begin = 0;
    for (uint32_t i = 0; i < c->graphs[0].nnodes; i++)
        if (c->graphs[0].nodes[i].kind == BPK_EVENT_BEGIN_PLAY) begin = c->graphs[0].nodes[i].id;
    if (!begin) begin = node(c, BPK_EVENT_BEGIN_PLAY, 0, 0, NULL);
    BpGraph* g = &c->graphs[0];
    uint32_t add = node(c, BPK_INV_ADD, 300, 0, NULL);
    BpNode* an = bp_node(g, add);
    if (an) {
        snprintf(an->pins[pin(g, add, 0, "Item")].slit, sizeof an->pins[0].slit, "DA_TestPotion");
        an->pins[pin(g, add, 0, "Count")].lit.v[0] = 2.0f;
    }
    link(g, begin, "", add, "");
    uint32_t used = node(c, BPK_EVENT_ITEM_USED, 0, 200, NULL), set_used = node(c, BPK_VAR_SET, 300, 200, "LastUsed");
    link(g, used, "", set_used, ""), link(g, used, "Item", set_used, "LastUsed");
    uint32_t eq = node(c, BPK_EVENT_ITEM_EQUIPPED, 0, 400, NULL), set_eq = node(c, BPK_VAR_SET, 300, 400, "EquippedSlot");
    link(g, eq, "", set_eq, ""), link(g, eq, "Slot Name", set_eq, "EquippedSlot");
    uint32_t ch = node(c, BPK_EVENT_INV_CHANGED, 0, 600, NULL), get_n = node(c, BPK_VAR_GET, 150, 650, "Changes");
    uint32_t inc = node(c, BPK_IADD, 250, 650, NULL), set_n = node(c, BPK_VAR_SET, 400, 600, "Changes");
    BpNode* incn = bp_node(g, inc);
    if (incn) incn->pins[1].lit.v[0] = 1.0f; // (Changes + 1)
    link(g, ch, "", set_n, ""), link(g, get_n, "Changes", inc, incn ? incn->pins[0].name : "A"), link(g, inc, incn ? incn->pins[2].name : "", set_n, "Changes");
    if (!bp_save(c) || !bp_compile(c)) {
        printf("[RealEngine] FAIL: inventory: BP_InvTester does not compile: %s\n", c->error);
        return false;
    }
    g_t = bp_spawn(w, c, (float[3]){ 0, Y, 100 }, (float[3]){ 0, 0, 0 });
    // the Character with the Mannequin (its parts per bone) and an Inventory
    g_hero = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH) | COMP_BIT(COMP_INVENTORY));
    const SkelMesh* mq = anim_mesh_name("SK_Mannequin");
    if (!g_t || !g_hero || !mq) {
        printf("[RealEngine] FAIL: inventory: the actors (the Mannequin: %s)\n", mq ? "found" : "missing");
        return false;
    }
    CTransform* ht = ecs_get(w, g_hero, COMP_TRANSFORM);
    ht->location[0] = -1500.0f, ht->location[1] = Y, ht->location[2] = 90.0f, ht->flags = MOBILITY_MOVABLE;
    CCharacter* cc = ecs_get(w, g_hero, COMP_CHARACTER);
    cc->max_walk_speed = 300.0f, cc->gravity_scale = 1.0f, cc->radius = 34.0f, cc->half_height = 88.0f;
    ((CSkeletalMesh*)ecs_get(w, g_hero, COMP_SKELETAL_MESH))->mesh = mq->hash;
    snprintf(((CName*)ecs_get(w, g_hero, COMP_NAME))->text, RE_NAME_MAX, "InvHero");
    Entity floor = editor_spawn_shape(ed, SHAPE_PLANE, (float[3]){ 0, Y, 0 }, 0.0);
    CTransform* ft = ecs_get(w, floor, COMP_TRANSFORM);
    if (ft) ft->scale[0] = 60.0f, ft->scale[1] = 20.0f, ft->location[2] = -1.0f, ft->flags = MOBILITY_STATIC;
    g_respawn = pickup(w, g_potion, 2, (float[3]){ 400, Y + 600, 30 }, PICKUP_ON_TOUCH, 0.4f);
    for (int i = 0; i < 256; i++) // (the measure: spinning, checked against both inventories every frame)
        pickup(w, g_rock, 1, (float[3]){ -1600.0f + (float)(i % 32) * 100.0f, Y + 1500.0f + (float)(i / 32) * 100.0f, 30 }, PICKUP_ON_TOUCH, 0.0f);
    ed->cam.pos[0] = -300.0f, ed->cam.pos[1] = Y - 1300.0f, ed->cam.pos[2] = 500.0f;
    ed->cam.yaw = 90.0f, ed->cam.pitch = -18.0f;
    ed->test_step = step;
    editor_play(ed, 0.0);
    return true;
}

bool inventory_test_check(Editor* ed) {
    (void)ed;
    if (g_state < 20) printf("[RealEngine] FAIL: inventory: stopped at step %d\n", g_state), g_ok = false;
    const InvStats* st = inv_stats();
    double tick = g_tick_n ? g_tick_ms / g_tick_n : 0.0;
    printf("[RealEngine] Inventory cost: inv_tick %.4f ms/frame (%u inventories, %u Pickups spinning and checked, %u equipped, %u items known, "
           "%u events sent); Add + Remove %.3f us; WBP_Inventory drawn in %.4f ms (all the widgets)\n",
           tick, st->inventories, st->pickups, st->equipped, st->items, st->events, g_op_us, g_draw_n ? g_draw_ms / g_draw_n : 0.0);
    CHECK(tick < 0.25, "inv_tick costs %.3f ms a frame (0.25 at most)", tick);
    CHECK(g_op_us < 20.0, "an Add + Remove costs %.2f us (20 at most)", g_op_us);
    editor_stop(ed, 0.0);
    g_done = true;
    if (g_ok) printf("[RealEngine] Inventory test: OK\n");
    return g_ok;
}
