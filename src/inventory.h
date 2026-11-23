// Inventory and Items (notas-dev/inventario.md; módulo 2 do R7): items are Data Assets of the ready-made type "Item
// Definition" (display name, description, icon, world mesh, stack size, weight, Gameplay Tags, equipment slot, value).
//   * the Inventory component (CInventory, components.h: Details > Add, or a Blueprint's components): slots, the most
//     weight it carries, starting items, picking up; its contents live here while playing (fixed pools: no malloc in the
//     frame), stacks fill before empty slots, move / swap / split / sort
//   * equipment: named slots (Right Hand, Head...), the item's world mesh hung on its socket or bone of the actor's
//     Skeletal Mesh (else on the actor)
//   * the Pickup actor (CPickup): the item in the world, taken on touch (overlap) or by the interact key / node
//   * the Blueprint nodes (bp_vm.c): Add Item, Remove Item, Has Item, Get Item Count, Use Item, Equip Item, Unequip Item,
//     Drop Item, Get Slot, Move Item, Find Item by Tag... and the events On Inventory Changed / Item Used / Equipped
//   * saved with Save Game to Slot (online.c): a record in the same file
// An item is named in the Blueprints and scripts by its asset's name ("DA_Potion"): a Name pin.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

#define INV_MAX_SLOTS 64     // a component's slots at most
#define INV_MAX_ITEMS 512    // Item Definitions known at once
#define INV_MAX_INVENTORIES 256 // inventories alive at once while playing
#define INV_ITEM_TAGS 8

// Equipment slots (Unreal's usual ones): their names are what an Item Definition's "Equip Slot" says
typedef enum InvEquipSlot {
    INV_EQ_RIGHT_HAND = 0, INV_EQ_LEFT_HAND, INV_EQ_HEAD, INV_EQ_CHEST, INV_EQ_LEGS, INV_EQ_FEET, INV_EQ_BACK, INV_EQ_ACCESSORY,
    INV_EQ_COUNT
} InvEquipSlot;
extern const char* const g_inv_equip_names[INV_EQ_COUNT];   // "Right Hand", "Head", ...
extern const char* const g_inv_equip_sockets[INV_EQ_COUNT]; // the bone each hangs from by default: "hand_r", "head", ...
extern const char* const g_inv_equip_labels[INV_EQ_COUNT];  // the UI's (TR: "Back Slot", not the menus' "Back")
int inv_equip_slot_of(const char* name); // -1: none / unknown (case and spaces don't matter: "RightHand")

// An Item Definition as the game reads it (from its Data Asset; made again when play starts)
typedef struct ItemDef {
    uint32_t hash;           // the asset's (its vpath's): what components, pickups and saves store
    uint32_t name_hash;      // its name's (the lookup by name)
    char name[48];           // the asset's name: "DA_Sword"
    char display[64];        // Display Name
    char description[160];
    char icon[160];          // a texture ("/Content/...png")
    char mesh_path[160];     // World Mesh: an imported mesh's vpath or a basic shape's name ("Cube", "Sphere"...)
    uint32_t mesh;           // its id for CStaticMesh (a shape's index or render_mesh_id_of)
    char socket[32];         // where it hangs equipped ("": the slot's default bone)
    int32_t equip;           // InvEquipSlot, -1: not equipment
    int32_t max_stack;       // 1: no stacking
    float weight;            // kg each
    int32_t value;           // coins each
    float attach_loc[3], attach_rot[3], attach_scale; // its place on the socket (cm, degrees; scale 0 read as 1)
    float color[3];          // the mesh's color when it has no material (the Base Color field)
    uint8_t ntags;
    bool consumable;         // tagged Item.Consumable (or under it): Use Item takes one
    char tags[INV_ITEM_TAGS][40];
} ItemDef;

// --- Item Definitions ---
// Every Data Asset of the type "Item Definition" read again (the Content folder scanned once); called by play's start
void inv_items_refresh(void);
uint32_t inv_item_count(void);
const ItemDef* inv_item_at(uint32_t i);
const ItemDef* inv_item_by_hash(uint32_t hash);
const ItemDef* inv_item_by_name(const char* name); // the asset's name ("DA_Potion") or its Display Name
bool inv_item_has_tag(const ItemDef* d, const char* query); // a Gameplay Tag match (gameplay_tags.h: "Item" has "Item.Weapon")
// The Data Asset's fields of an Item Definition (blueprint.c bp_create: a new one gets them; an older one missing one
// reads its default)
struct BpAsset;
void inv_item_fields(struct BpAsset* a);
bool inv_is_item_asset(const struct BpAsset* a);

// --- Play ---
void inv_begin_play(World* w); // the inventories made with their starting items (before any BeginPlay)
void inv_tick(World* w, float dt); // pickups touched / interacted, spinning pickups, equipment, the events sent
void inv_end_play(void);

// --- Operations (the nodes'): e is the actor with the Inventory component. Counts <= 0 read as 1 ---
int inv_add(World* w, Entity e, uint32_t item, int count);    // how many went in (the rest: no room or too heavy)
int inv_remove(World* w, Entity e, uint32_t item, int count); // how many came out (from the last stacks)
int inv_count(World* w, Entity e, uint32_t item);              // in the slots (not the equipped)
bool inv_use(World* w, Entity e, uint32_t item);               // On Item Used; a consumable spends one
bool inv_use_slot(World* w, Entity e, int slot);               // the same, a slot's item (WBP_Inventory's right click)
bool inv_equip(World* w, Entity e, uint32_t item);             // from the slots onto its equipment slot (the one there back)
bool inv_equip_slot(World* w, Entity e, int slot);             // the same, a slot's item (WBP_Inventory's double click)
bool inv_unequip(World* w, Entity e, int slot);                // back into the slots (false: no room)
uint32_t inv_equipped(World* w, Entity e, int slot);           // the item there (0: none)
Entity inv_equipped_entity(World* w, Entity e, int slot);      // its mesh hung on the actor (ENTITY_NONE: none)
Entity inv_drop(World* w, Entity e, uint32_t item, int count); // a Pickup in front of the actor (ENTITY_NONE: none had)
bool inv_get_slot(World* w, Entity e, int slot, uint32_t* item, int* count);
bool inv_move(World* w, Entity e, int from, int to);           // merged when the same item (up to its stack), else swapped
bool inv_transfer(World* w, Entity from, int from_slot, Entity to, int to_slot); // between two inventories (a chest)
bool inv_split(World* w, Entity e, int slot, int count);       // count of the stack into the first empty slot
void inv_sort(World* w, Entity e);                             // stacks merged, by display name, empty slots last
bool inv_find_tag(World* w, Entity e, const char* tag, uint32_t* item, int* slot);
float inv_weight(World* w, Entity e, float* max_weight);       // kg in the slots and equipped; *max 0: no limit
int inv_slot_count(World* w, Entity e);                        // 0: no Inventory component
bool inv_pick_up(World* w, Entity e, Entity pickup);           // the Pickup's item into e's inventory (the Pickup goes)
void inv_clear(World* w, Entity e);                            // empty (the tests)
uint32_t inv_revision(World* w, Entity e);                     // bumped by every change (the widgets redraw)

// --- Saves (online.c: Save Game to Slot) ---
// The actor's inventory and equipment as a record appended to a save (its variables' stream, bp_vars_dump's format):
// the new length (len when it has no inventory or no room). Read back by name from a save; false: none in it
uint32_t inv_save_append(World* w, Entity e, uint8_t* buf, uint32_t len, uint32_t cap);
bool inv_save_read(World* w, Entity e, const uint8_t* buf, uint32_t len);

// --- Editor / tests ---
// WBP_Inventory (Content/UI, made when missing): a panel with the title, the weight and the grid of slots; its hash
uint32_t inv_widget_asset(void);
typedef struct InvStats {
    uint32_t inventories, pickups, equipped, items, events; // alive now; events sent since the start of play
    double tick_ms;          // inv_tick's last frame
    double op_us;            // the last operation's (an Add, a Move...)
} InvStats;
const InvStats* inv_stats(void);

// --- WBP_Inventory's grid (inventory_widget.c, beside UMG's drawing: the editor and the game) ---
struct ImDrawList;
struct UiWidget;
// The frame's mouse against the grids the last frame drew (ui_draw_viewport, before the trees): drag and drop, right
// click (use), double click (equip / unequip)
void inv_widget_frame(World* w, const float mouse[2], bool down, bool clicked, bool rclicked, bool dclicked);
// An Inventory Grid widget in its rectangle (designer: a preview with made-up items)
void inv_widget_draw(struct ImDrawList* dl, const struct UiWidget* x, float x0, float y0, float x1, float y1, float k, float alpha, bool designer);
void inv_widget_overlay(struct ImDrawList* dl); // the dragged item and the tip, over every widget
bool inv_widget_busy(void);                    // a slot pressed or dragged
// The tests: a mouse of their own (on: instead of the real one; a click counts once) and a slot's center on the screen
void inv_widget_test_input(bool on, float x, float y, bool down, bool clicked, bool rclicked);
bool inv_widget_cell(Entity owner, bool equip, int slot, float xy[2]);

// --- The editor's Details (inventory_ui.c: the level's Details, a Blueprint's components) ---
struct CInventory;
struct CPickup;
struct CStaticMesh;
bool inv_ui_item_combo(const char* label, uint32_t* item);
bool inv_ui_inventory(struct CInventory* c);
bool inv_ui_pickup(struct CPickup* p, struct CStaticMesh* sm, float scale[3]); // (sm, scale: the actor's, made the item's)
