// Inventory and Items (inventory.h, notas-dev/inventario.md): the Item Definitions read from their Data Assets, the
// inventories' contents while playing (a fixed pool: nothing allocated in the frame), the stacks, the weight, the
// equipment hung on the bones, the Pickups in the world, the events into the Blueprints and the saves' record.
#include "inventory.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "anim.h"
#include "blueprint.h"
#include "bp_internal.h"
#include "components.h"
#include "gameplay_tags.h"
#include "platform.h"
#include "platform_files.h"
#include "render.h"
#include "ui_widget.h"

const char* const g_inv_equip_names[INV_EQ_COUNT] = { "Right Hand", "Left Hand", "Head", "Chest", "Legs", "Feet", "Back", "Accessory" };
const char* const g_inv_equip_labels[INV_EQ_COUNT] = { "Right Hand", "Left Hand", "Head", "Chest", "Legs", "Feet", "Back Slot", "Accessory" };
const char* const g_inv_equip_sockets[INV_EQ_COUNT] = { "hand_r", "hand_l", "head", "spine_03", "pelvis", "foot_r", "spine_03", "neck_01" };

#define TABLE 1024 // (twice the items: open addressing)
#define MAX_EV 128
#define MAX_TAKE 64

typedef struct InvSlot { uint32_t item; int32_t count; } InvSlot;
typedef struct Inv {
    Entity owner;                 // ENTITY_NONE: a free entry
    uint32_t nslots;
    InvSlot s[INV_MAX_SLOTS];
    uint32_t eq[INV_EQ_COUNT];    // the equipped items (0: none)
    Entity eq_ent[INV_EQ_COUNT];  // their meshes on the actor
    uint32_t rev;
    bool changed;                 // On Inventory Changed due
} Inv;
typedef struct InvEv { Entity owner; uint16_t kind; uint8_t slot; uint32_t item; } InvEv;

static struct {
    ItemDef items[INV_MAX_ITEMS];
    uint32_t nitems;
    uint16_t by_hash[TABLE], by_name[TABLE]; // an item's index + 1 (0: empty)
    bool ready;
    Inv inv[INV_MAX_INVENTORIES];
    uint32_t ninv;                // the high-water mark
    bool playing;
    World* w;
    InvEv ev[MAX_EV];
    uint32_t nev;
    bool key_was;
    InvStats stats;
} I;

static uint32_t lhash(const char* s) { // FNV-1a of the lower case name, never 0
    uint32_t h = 2166136261u;
    for (; s && *s; s++) h = (h ^ (uint8_t)tolower((unsigned char)*s)) * 16777619u;
    return h ? h : 1u;
}
static bool same_loose(const char* a, const char* b) { // case, spaces and '_' ignored ("RightHand" is "Right Hand")
    for (;;) {
        while (*a == ' ' || *a == '_') a++;
        while (*b == ' ' || *b == '_') b++;
        if (tolower((unsigned char)*a) != tolower((unsigned char)*b)) return false;
        if (!*a) return true;
        a++, b++;
    }
}
int inv_equip_slot_of(const char* name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < INV_EQ_COUNT; i++)
        if (same_loose(name, g_inv_equip_names[i])) return i;
    return -1;
}

// --- Item Definitions ---

bool inv_is_item_asset(const BpAsset* a) { return a && a->kind == BPA_DATA && !_stricmp(a->parent, "Item Definition"); }

// The fields a new Item Definition starts with (its Data Asset's values; more of the user's own can follow)
void inv_item_fields(BpAsset* a) {
    static const struct { const char* name; uint8_t type; float v[3]; const char* s; } f[] = {
        { "Display Name", BPT_TEXT, { 0 }, "New Item" },
        { "Description", BPT_TEXT, { 0 }, "" },
        { "Icon", BPT_STRING, { 0 }, "" },
        { "World Mesh", BPT_STRING, { 0 }, "Cube" },
        { "World Scale", BPT_FLOAT, { 0.25f }, NULL },
        { "Base Color", BPT_COLOR, { 0.8f, 0.6f, 0.2f }, NULL },
        { "Max Stack", BPT_INT, { 1 }, NULL },
        { "Weight", BPT_FLOAT, { 1.0f }, NULL },
        { "Value", BPT_INT, { 10 }, NULL },
        { "Tags", BPT_STRING, { 0 }, "Item" },
        { "Equip Slot", BPT_NAME, { 0 }, "" },
        { "Socket", BPT_NAME, { 0 }, "" },
        { "Attach Location", BPT_VECTOR, { 0 }, NULL },
        { "Attach Rotation", BPT_ROTATOR, { 0 }, NULL },
    };
    for (size_t i = 0; i < sizeof f / sizeof f[0]; i++) {
        if (bp_var_index(a, f[i].name) >= 0) continue;
        int k = bp_add_var(a, f[i].name, f[i].type);
        if (k < 0) break;
        BpVar* v = &a->vars[k];
        memcpy(v->def.v, f[i].v, sizeof f[i].v);
        if (f[i].type == BPT_COLOR) v->def.v[3] = 1.0f;
        if (f[i].s) snprintf(v->sdef, sizeof v->sdef, "%s", f[i].s);
        snprintf(v->category, sizeof v->category, "%s", i < 6 ? "Display" : i < 10 ? "Rules" : "Equipment");
    }
}

static const BpVar* field(const BpAsset* a, const char* name) {
    int k = bp_var_index(a, name);
    return k >= 0 ? &a->vars[k] : NULL;
}
static const char* fstr(const BpAsset* a, const char* name, const char* def) {
    const BpVar* v = field(a, name);
    return v && bp_type_stringish(v->type) ? v->sdef : def;
}
static float fnum(const BpAsset* a, const char* name, float def) {
    const BpVar* v = field(a, name);
    return v && !bp_type_stringish(v->type) ? v->def.v[0] : def;
}
static void fvec(const BpAsset* a, const char* name, float out[3], const float def[3]) {
    const BpVar* v = field(a, name);
    memcpy(out, v && !bp_type_stringish(v->type) ? v->def.v : def, 12);
}

static uint32_t mesh_of(const char* s) { // a shape's name, an imported mesh's vpath or its name
    if (!s || !s[0]) return SHAPE_CUBE;
    for (uint32_t i = 0; i < SHAPE_COUNT; i++)
        if (!_stricmp(s, g_shape_names[i])) return i;
    if (s[0] == '/') return render_mesh_id_of(s);
    for (uint32_t i = SHAPE_COUNT; i < render_mesh_count(); i++) {
        const char* n = render_mesh_name(i);
        if (!n) continue;
        const char* b = strrchr(n, '/');
        b = b ? b + 1 : n;
        size_t l = strlen(s);
        if (!_stricmp(n, s) || (!_strnicmp(b, s, l) && (b[l] == 0 || b[l] == '.'))) return render_mesh_id(i);
    }
    return SHAPE_CUBE;
}

static void parse_tags(ItemDef* d, const char* s) {
    d->ntags = 0;
    while (*s && d->ntags < INV_ITEM_TAGS) {
        while (*s == ',' || *s == ';' || *s == ' ' || *s == '|') s++;
        size_t n = 0;
        while (s[n] && s[n] != ',' && s[n] != ';' && s[n] != ' ' && s[n] != '|') n++;
        if (!n) break;
        char raw[64];
        size_t c = n < sizeof raw - 1 ? n : sizeof raw - 1;
        memcpy(raw, s, c), raw[c] = 0;
        if (gt_clean(raw, d->tags[d->ntags], sizeof d->tags[0])) d->ntags++;
        s += n;
    }
}

static void table_put(uint16_t* t, uint32_t h, uint16_t idx) {
    for (uint32_t i = h & (TABLE - 1);; i = (i + 1) & (TABLE - 1))
        if (!t[i]) {
            t[i] = idx;
            return;
        }
}

void inv_items_refresh(void) {
    bp_find_name(BPA_DATA, "\x01"); // (the Content folder scanned once: every asset known)
    I.nitems = 0;
    memset(I.by_hash, 0, sizeof I.by_hash), memset(I.by_name, 0, sizeof I.by_name);
    static const float zero[3] = { 0, 0, 0 }, gold[3] = { 0.8f, 0.6f, 0.2f };
    for (uint32_t i = 0; i < bp_asset_count() && I.nitems < INV_MAX_ITEMS; i++) {
        const BpAsset* a = bp_asset_at(i);
        if (!inv_is_item_asset(a)) continue;
        ItemDef* d = &I.items[I.nitems];
        memset(d, 0, sizeof *d);
        d->hash = a->hash;
        snprintf(d->name, sizeof d->name, "%s", a->name);
        d->name_hash = lhash(a->name);
        snprintf(d->display, sizeof d->display, "%s", fstr(a, "Display Name", a->name));
        if (!d->display[0]) snprintf(d->display, sizeof d->display, "%s", a->name);
        snprintf(d->description, sizeof d->description, "%s", fstr(a, "Description", ""));
        snprintf(d->icon, sizeof d->icon, "%s", fstr(a, "Icon", ""));
        snprintf(d->mesh_path, sizeof d->mesh_path, "%s", fstr(a, "World Mesh", "Cube"));
        d->mesh = mesh_of(d->mesh_path);
        snprintf(d->socket, sizeof d->socket, "%s", fstr(a, "Socket", ""));
        d->equip = inv_equip_slot_of(fstr(a, "Equip Slot", ""));
        int ms = (int)fnum(a, "Max Stack", 1.0f);
        d->max_stack = ms < 1 ? 1 : ms > 9999 ? 9999 : ms;
        d->weight = fmaxf(fnum(a, "Weight", 0.0f), 0.0f);
        d->value = (int)fnum(a, "Value", 0.0f);
        d->attach_scale = fnum(a, "World Scale", 0.25f);
        if (d->attach_scale <= 0.0f) d->attach_scale = 1.0f;
        fvec(a, "Attach Location", d->attach_loc, zero);
        fvec(a, "Attach Rotation", d->attach_rot, zero);
        fvec(a, "Base Color", d->color, gold);
        parse_tags(d, fstr(a, "Tags", ""));
        d->consumable = inv_item_has_tag(d, "Item.Consumable");
        I.nitems++;
        table_put(I.by_hash, d->hash, (uint16_t)I.nitems);
        table_put(I.by_name, d->name_hash, (uint16_t)I.nitems);
    }
    I.ready = true;
    I.stats.items = I.nitems;
}

static void ready(void) {
    if (!I.ready) inv_items_refresh();
}
uint32_t inv_item_count(void) {
    ready();
    return I.nitems;
}
const ItemDef* inv_item_at(uint32_t i) {
    ready();
    return i < I.nitems ? &I.items[i] : NULL;
}
const ItemDef* inv_item_by_hash(uint32_t hash) {
    if (!hash) return NULL;
    ready();
    for (uint32_t i = hash & (TABLE - 1);; i = (i + 1) & (TABLE - 1)) {
        uint16_t k = I.by_hash[i];
        if (!k) return NULL;
        if (I.items[k - 1].hash == hash) return &I.items[k - 1];
    }
}
const ItemDef* inv_item_by_name(const char* name) {
    if (!name || !name[0] || !_stricmp(name, "None")) return NULL;
    ready();
    uint32_t h = lhash(name);
    for (uint32_t i = h & (TABLE - 1);; i = (i + 1) & (TABLE - 1)) {
        uint16_t k = I.by_name[i];
        if (!k) break;
        if (I.items[k - 1].name_hash == h && !_stricmp(I.items[k - 1].name, name)) return &I.items[k - 1];
    }
    for (uint32_t i = 0; i < I.nitems; i++) // (its Display Name: "Health Potion")
        if (!_stricmp(I.items[i].display, name)) return &I.items[i];
    return NULL;
}
bool inv_item_has_tag(const ItemDef* d, const char* query) {
    if (!d || !query || !query[0]) return false;
    for (uint32_t t = 0; t < d->ntags; t++)
        if (gt_matches(d->tags[t], query, false)) return true;
    return false;
}

// --- The inventories ---

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }
static int max_of(uint32_t item) {
    const ItemDef* d = inv_item_by_hash(item);
    return d ? d->max_stack : 1;
}
static float weight_of(const Inv* v) {
    float s = 0.0f;
    for (uint32_t i = 0; i < v->nslots; i++)
        if (v->s[i].item) {
            const ItemDef* d = inv_item_by_hash(v->s[i].item);
            if (d) s += d->weight * (float)v->s[i].count;
        }
    for (int e = 0; e < INV_EQ_COUNT; e++) {
        const ItemDef* d = inv_item_by_hash(v->eq[e]);
        if (d) s += d->weight;
    }
    return s;
}
static void touched(Inv* v) {
    v->rev++;
    v->changed = true;
}
static void post(Entity owner, uint16_t kind, uint32_t item, int slot) {
    if (I.nev < MAX_EV) I.ev[I.nev++] = (InvEv){ owner, kind, (uint8_t)slot, item };
}

// What fits of item (room in the stacks and the slots, the weight left), put in; no events
static int add_raw(Inv* v, float max_w, uint32_t item, int count) {
    const ItemDef* d = inv_item_by_hash(item);
    if (!d || count <= 0) return 0;
    int left = count;
    if (max_w > 0.0f && d->weight > 0.0f) {
        int can = (int)floorf((max_w - weight_of(v)) / d->weight + 1e-4f);
        if (can < left) left = can < 0 ? 0 : can;
    }
    int want = left;
    for (uint32_t i = 0; i < v->nslots && left > 0; i++) // the stacks of it first
        if (v->s[i].item == item && v->s[i].count < d->max_stack) {
            int t = d->max_stack - v->s[i].count;
            t = t < left ? t : left;
            v->s[i].count += t, left -= t;
        }
    for (uint32_t i = 0; i < v->nslots && left > 0; i++) // then empty slots
        if (!v->s[i].item) {
            int t = d->max_stack < left ? d->max_stack : left;
            v->s[i].item = item, v->s[i].count = t, left -= t;
        }
    return want - left;
}

static Inv* get(World* w, Entity e) {
    if (!w || !ecs_alive(w, e)) return NULL;
    CInventory* c = ecs_get(w, e, COMP_INVENTORY);
    if (!c) { // (an Actor Component Blueprint's self: its actor's)
        Entity o = bp_component_owner(w, e);
        if (o != e && ecs_alive(w, o)) e = o, c = ecs_get(w, e, COMP_INVENTORY);
    }
    if (!c) return NULL;
    if (c->handle && c->handle <= INV_MAX_INVENTORIES && I.inv[c->handle - 1].owner == e) return &I.inv[c->handle - 1];
    if (!I.playing) return NULL; // (contents exist while playing only: the level keeps the settings and starting items)
    uint32_t k = 0;
    while (k < I.ninv && I.inv[k].owner != ENTITY_NONE) k++;
    if (k >= INV_MAX_INVENTORIES) return NULL;
    if (k == I.ninv) I.ninv++;
    Inv* v = &I.inv[k];
    memset(v, 0, sizeof *v);
    v->owner = e;
    v->nslots = (uint32_t)clampi((int)c->slots, 1, INV_MAX_SLOTS);
    c->handle = k + 1;
    for (int s = 0; s < INV_START_ITEMS; s++)
        if (c->start_item[s]) add_raw(v, c->max_weight, c->start_item[s], c->start_count[s] ? (int)c->start_count[s] : 1);
    v->rev = 1;
    return v;
}
static float max_w(World* w, const Inv* v) {
    const CInventory* c = ecs_get(w, v->owner, COMP_INVENTORY);
    return c ? c->max_weight : 0.0f;
}
static int cnt(int c) { return c <= 0 ? 1 : c; }

int inv_add(World* w, Entity e, uint32_t item, int count) {
    double t0 = re_time_seconds();
    Inv* v = get(w, e);
    int n = v ? add_raw(v, max_w(w, v), item, cnt(count)) : 0;
    if (n) touched(v);
    I.stats.op_us = (re_time_seconds() - t0) * 1e6;
    return n;
}

int inv_remove(World* w, Entity e, uint32_t item, int count) {
    Inv* v = get(w, e);
    if (!v || !item) return 0;
    int left = cnt(count);
    for (int i = (int)v->nslots - 1; i >= 0 && left > 0; i--)
        if (v->s[i].item == item) {
            int t = v->s[i].count < left ? v->s[i].count : left;
            v->s[i].count -= t, left -= t;
            if (!v->s[i].count) v->s[i].item = 0;
        }
    int n = cnt(count) - left;
    if (n) touched(v);
    return n;
}

int inv_count(World* w, Entity e, uint32_t item) {
    Inv* v = get(w, e);
    int n = 0;
    for (uint32_t i = 0; v && item && i < v->nslots; i++)
        if (v->s[i].item == item) n += v->s[i].count;
    return n;
}

static bool use_slot(Inv* v, int i) {
    if (i < 0 || i >= (int)v->nslots || !v->s[i].item) return false;
    uint32_t item = v->s[i].item;
    const ItemDef* d = inv_item_by_hash(item);
    post(v->owner, BPK_EVENT_ITEM_USED, item, i);
    if (d && d->consumable && --v->s[i].count <= 0) v->s[i].item = 0, v->s[i].count = 0;
    touched(v);
    return true;
}
bool inv_use(World* w, Entity e, uint32_t item) {
    Inv* v = get(w, e);
    for (uint32_t i = 0; v && item && i < v->nslots; i++)
        if (v->s[i].item == item) return use_slot(v, (int)i);
    return false;
}
bool inv_use_slot(World* w, Entity e, int slot) {
    Inv* v = get(w, e);
    return v && use_slot(v, slot);
}

// The equipped item's mesh hung on the actor: on its socket's bone (the Skeletal Mesh part that bone moves, else the
// nearest above), else on the Skeletal Mesh, else on the actor itself; a Blueprint component's way (CAttach, placed by
// bp_components_update each frame, gone with the actor)
static Entity hang(World* w, Entity owner, const ItemDef* d, int slot) {
    const char* sock = d->socket[0] ? d->socket : g_inv_equip_sockets[slot];
    Entity parent = anim_bone_entity(w, owner, sock);
    if (parent == ENTITY_NONE) parent = anim_mesh_entity(w, owner);
    if (parent == ENTITY_NONE || !ecs_alive(w, parent)) parent = owner;
    uint32_t node = 1;
    const CAttach* pa = ecs_get(w, parent, COMP_ATTACH);
    if (pa && pa->kind == ATTACH_SCS && pa->node + 1 < BP_MAX_SCS) node = pa->node + 1;
    Entity m = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
    if (m == ENTITY_NONE) return ENTITY_NONE;
    snprintf(((CName*)ecs_get(w, m, COMP_NAME))->text, RE_NAME_MAX, "%s (%s)", d->display, g_inv_equip_names[slot]);
    CStaticMesh* sm = ecs_get(w, m, COMP_STATIC_MESH);
    sm->mesh = d->mesh;
    memcpy(sm->base_color, d->color, 12);
    sm->roughness = 0.45f, sm->cast_shadow = 1;
    CTransform* t = ecs_get(w, m, COMP_TRANSFORM);
    const CTransform* ot = ecs_get(w, owner, COMP_TRANSFORM);
    if (ot) memcpy(t->location, ot->location, 12);
    t->flags = MOBILITY_MOVABLE | TRANSFORM_NO_COLLISION;
    CAttach* at = ecs_get(w, m, COMP_ATTACH);
    at->owner = owner, at->parent = parent, at->node = node, at->kind = ATTACH_SCS;
    memcpy(at->location, d->attach_loc, 12), memcpy(at->rotation, d->attach_rot, 12);
    at->scale[0] = at->scale[1] = at->scale[2] = d->attach_scale;
    // (Q10) on the Skeletal Mesh's socket itself (its bone and its offset, the Skeleton tab's), placed by the pose: a
    // skinned character too (it has no parts to hang from), the ragdoll's and the dynamic bones' pose included
    anim_attach_to_socket(w, m, owner, sock);
    return m;
}
static void unhang(World* w, Inv* v, int slot) {
    if (v->eq_ent[slot] && ecs_alive(w, v->eq_ent[slot])) ecs_destroy(w, v->eq_ent[slot]);
    v->eq_ent[slot] = ENTITY_NONE;
}

static bool equip_slot(World* w, Inv* v, int i) {
    if (i < 0 || i >= (int)v->nslots || !v->s[i].item) return false;
    uint32_t item = v->s[i].item;
    const ItemDef* d = inv_item_by_hash(item);
    if (!d || d->equip < 0) return false;
    int es = d->equip;
    uint32_t old = v->eq[es];
    if (--v->s[i].count <= 0) v->s[i].item = 0, v->s[i].count = 0;
    if (old) { // what was there back into the slots: where this one was, else wherever it fits
        if (!v->s[i].item) v->s[i].item = old, v->s[i].count = 1;
        else if (add_raw(v, 0.0f, old, 1) != 1) { // (no room: nothing changes)
            v->s[i].item = item, v->s[i].count++;
            return false;
        }
        unhang(w, v, es);
        post(v->owner, BPK_EVENT_ITEM_UNEQUIPPED, old, es);
    }
    v->eq[es] = item;
    v->eq_ent[es] = hang(w, v->owner, d, es);
    post(v->owner, BPK_EVENT_ITEM_EQUIPPED, item, es);
    touched(v);
    return true;
}
bool inv_equip(World* w, Entity e, uint32_t item) {
    Inv* v = get(w, e);
    for (uint32_t i = 0; v && item && i < v->nslots; i++)
        if (v->s[i].item == item) return equip_slot(w, v, (int)i);
    return false;
}
bool inv_equip_slot(World* w, Entity e, int slot) {
    Inv* v = get(w, e);
    return v && equip_slot(w, v, slot);
}
bool inv_unequip(World* w, Entity e, int slot) {
    Inv* v = get(w, e);
    if (!v || slot < 0 || slot >= INV_EQ_COUNT || !v->eq[slot]) return false;
    uint32_t item = v->eq[slot];
    if (add_raw(v, 0.0f, item, 1) != 1) return false; // (already carried: its weight counted)
    v->eq[slot] = 0;
    unhang(w, v, slot);
    post(v->owner, BPK_EVENT_ITEM_UNEQUIPPED, item, slot);
    touched(v);
    return true;
}
uint32_t inv_equipped(World* w, Entity e, int slot) {
    Inv* v = get(w, e);
    return v && slot >= 0 && slot < INV_EQ_COUNT ? v->eq[slot] : 0;
}
Entity inv_equipped_entity(World* w, Entity e, int slot) {
    Inv* v = get(w, e);
    return v && slot >= 0 && slot < INV_EQ_COUNT ? v->eq_ent[slot] : ENTITY_NONE;
}

// A Pickup actor of the item: its Static Mesh the item's World Mesh; no collision (walked through, taken)
static Entity spawn_pickup(World* w, const ItemDef* d, int count, const float at[3], float yaw) {
    Entity p = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_PICKUP));
    if (p == ENTITY_NONE) return ENTITY_NONE;
    snprintf(((CName*)ecs_get(w, p, COMP_NAME))->text, RE_NAME_MAX, "Pickup_%s", d->name);
    CTransform* t = ecs_get(w, p, COMP_TRANSFORM);
    memcpy(t->location, at, 12);
    t->rotation[1] = yaw;
    t->scale[0] = t->scale[1] = t->scale[2] = d->attach_scale;
    t->flags = MOBILITY_MOVABLE | TRANSFORM_NO_COLLISION | TRANSFORM_NO_ICON;
    CStaticMesh* sm = ecs_get(w, p, COMP_STATIC_MESH);
    sm->mesh = d->mesh;
    memcpy(sm->base_color, d->color, 12);
    sm->roughness = 0.45f, sm->cast_shadow = 1;
    CPickup* pk = ecs_get(w, p, COMP_PICKUP);
    memset(pk, 0, sizeof *pk);
    pk->item = d->hash, pk->count = (uint32_t)count, pk->full = (uint32_t)count, pk->spin = 1;
    return p;
}

Entity inv_drop(World* w, Entity e, uint32_t item, int count) {
    Inv* v = get(w, e);
    const ItemDef* d = inv_item_by_hash(item);
    if (!v || !d) return ENTITY_NONE;
    int n = inv_remove(w, v->owner, item, count);
    if (!n) return ENTITY_NONE;
    const CTransform* t = ecs_get(w, v->owner, COMP_TRANSFORM);
    const CCharacter* ch = ecs_get(w, v->owner, COMP_CHARACTER);
    float yaw = ch ? ch->control_yaw : t ? t->rotation[1] : 0.0f, at[3] = { 0, 0, 0 };
    if (t) {
        float r = yaw * 0.0174532925f, ahead = 110.0f;
        at[0] = t->location[0] + cosf(r) * ahead, at[1] = t->location[1] + sinf(r) * ahead;
        at[2] = t->location[2] - (ch ? ch->half_height : 0.0f) + 25.0f; // (by the feet)
    }
    Entity p = spawn_pickup(w, d, n, at, yaw);
    CPickup* pk = p ? ecs_get(w, p, COMP_PICKUP) : NULL;
    if (pk) pk->ignore = v->owner; // (until it walks away: not taken back at once)
    return p;
}

bool inv_get_slot(World* w, Entity e, int slot, uint32_t* item, int* count) {
    Inv* v = get(w, e);
    bool ok = v && slot >= 0 && slot < (int)v->nslots;
    if (item) *item = ok ? v->s[slot].item : 0;
    if (count) *count = ok ? v->s[slot].count : 0;
    return ok && v->s[slot].item;
}

// a onto b: merged up to b's stack (the same item), else swapped
static bool move_between(Inv* a, int from, Inv* b, int to, float bmax) {
    if (from < 0 || from >= (int)a->nslots || to < 0 || to >= (int)b->nslots || !a->s[from].item || (a == b && from == to)) return false;
    InvSlot* f = &a->s[from];
    InvSlot* t = &b->s[to];
    const ItemDef* d = inv_item_by_hash(f->item);
    if (a != b && bmax > 0.0f && d && d->weight > 0.0f) { // (another's weight limit)
        float room = bmax - weight_of(b);
        if (t->item && t->item != f->item) { const ItemDef* dt = inv_item_by_hash(t->item); room += dt ? dt->weight * (float)t->count : 0.0f; }
        if (d->weight * (float)(t->item == f->item ? 1 : f->count) > room + 1e-4f) return false;
    }
    if (t->item == f->item) {
        int m = max_of(f->item), n = m - t->count;
        if (n <= 0) return false;
        n = n < f->count ? n : f->count;
        if (a != b && bmax > 0.0f && d && d->weight > 0.0f) {
            int can = (int)floorf((bmax - weight_of(b)) / d->weight + 1e-4f);
            n = n < can ? n : can;
            if (n <= 0) return false;
        }
        t->count += n, f->count -= n;
        if (!f->count) f->item = 0;
    } else {
        InvSlot x = *t;
        *t = *f, *f = x;
    }
    touched(a);
    if (a != b) touched(b);
    return true;
}
bool inv_move(World* w, Entity e, int from, int to) {
    Inv* v = get(w, e);
    return v && move_between(v, from, v, to, 0.0f);
}
bool inv_transfer(World* w, Entity from, int from_slot, Entity to, int to_slot) {
    Inv* a = get(w, from);
    Inv* b = get(w, to);
    if (!a || !b || from_slot < 0 || from_slot >= (int)a->nslots || !a->s[from_slot].item) return false;
    if (a == b) return to_slot >= 0 && move_between(a, from_slot, a, to_slot, 0.0f);
    if (to_slot >= 0) return move_between(a, from_slot, b, to_slot, max_w(w, b));
    InvSlot* f = &a->s[from_slot]; // (-1: wherever it fits)
    int n = add_raw(b, max_w(w, b), f->item, f->count);
    if (!n) return false;
    if (!(f->count -= n)) f->item = 0;
    touched(a), touched(b);
    return true;
}

bool inv_split(World* w, Entity e, int slot, int count) {
    Inv* v = get(w, e);
    if (!v || slot < 0 || slot >= (int)v->nslots || v->s[slot].count < 2) return false;
    int n = count <= 0 ? v->s[slot].count / 2 : count;
    if (n >= v->s[slot].count) return false;
    for (uint32_t i = 0; i < v->nslots; i++)
        if (!v->s[i].item) {
            v->s[i].item = v->s[slot].item, v->s[i].count = n;
            v->s[slot].count -= n;
            touched(v);
            return true;
        }
    return false;
}

static int slot_order(const InvSlot* a, const InvSlot* b) { // empty last, then by display name, the bigger stack first
    if (!a->item || !b->item) return !a->item - !b->item;
    const ItemDef *da = inv_item_by_hash(a->item), *db = inv_item_by_hash(b->item);
    int c = _stricmp(da ? da->display : "", db ? db->display : "");
    if (c) return c;
    if (a->item != b->item) return a->item < b->item ? -1 : 1;
    return b->count - a->count;
}
void inv_sort(World* w, Entity e) {
    Inv* v = get(w, e);
    if (!v) return;
    for (uint32_t i = 0; i < v->nslots; i++) // the stacks of an item merged into its first ones
        for (uint32_t j = i + 1; v->s[i].item && j < v->nslots; j++)
            if (v->s[j].item == v->s[i].item) {
                int m = max_of(v->s[i].item), n = m - v->s[i].count;
                n = n < v->s[j].count ? n : v->s[j].count;
                v->s[i].count += n, v->s[j].count -= n;
                if (!v->s[j].count) v->s[j].item = 0;
            }
    for (uint32_t i = 1; i < v->nslots; i++) { // (insertion: 64 slots at most)
        InvSlot x = v->s[i];
        int j = (int)i - 1;
        while (j >= 0 && slot_order(&v->s[j], &x) > 0) v->s[j + 1] = v->s[j], j--;
        v->s[j + 1] = x;
    }
    touched(v);
}

bool inv_find_tag(World* w, Entity e, const char* tag, uint32_t* item, int* slot) {
    Inv* v = get(w, e);
    for (uint32_t i = 0; v && i < v->nslots; i++)
        if (v->s[i].item && inv_item_has_tag(inv_item_by_hash(v->s[i].item), tag)) {
            if (item) *item = v->s[i].item;
            if (slot) *slot = (int)i;
            return true;
        }
    if (item) *item = 0;
    if (slot) *slot = -1;
    return false;
}
float inv_weight(World* w, Entity e, float* mx) {
    Inv* v = get(w, e);
    if (mx) *mx = v ? max_w(w, v) : 0.0f;
    return v ? weight_of(v) : 0.0f;
}
int inv_slot_count(World* w, Entity e) {
    Inv* v = get(w, e);
    return v ? (int)v->nslots : 0;
}
void inv_clear(World* w, Entity e) {
    Inv* v = get(w, e);
    if (!v) return;
    memset(v->s, 0, sizeof v->s);
    for (int s = 0; s < INV_EQ_COUNT; s++) unhang(w, v, s), v->eq[s] = 0;
    touched(v);
}
uint32_t inv_revision(World* w, Entity e) {
    Inv* v = get(w, e);
    return v ? v->rev : 0;
}

// the Pickup's item into v (what doesn't fit stays in it); taken whole: gone, or hidden until it comes back
static bool take(World* w, Inv* v, Entity p) {
    CPickup* pk = ecs_get(w, p, COMP_PICKUP);
    CTransform* t = ecs_get(w, p, COMP_TRANSFORM);
    if (!pk || !t || (t->flags & TRANSFORM_HIDDEN_GAME)) return false;
    int n = add_raw(v, max_w(w, v), pk->item, pk->count ? (int)pk->count : 1);
    if (!n) return false;
    touched(v);
    int left = (pk->count ? (int)pk->count : 1) - n;
    if (left > 0) {
        pk->count = (uint32_t)left;
        return true;
    }
    if (pk->respawn_time > 0.0f) {
        pk->timer = pk->respawn_time;
        t->flags |= TRANSFORM_HIDDEN_GAME;
        ecs_touch(w, p);
    } else ecs_destroy(w, p);
    return true;
}
bool inv_pick_up(World* w, Entity e, Entity pickup) {
    Inv* v = get(w, e);
    return v && ecs_alive(w, pickup) && take(w, v, pickup);
}

// --- Play ---

void inv_begin_play(World* w) {
    I.playing = true;
    I.w = w;
    I.ninv = 0, I.nev = 0, I.key_was = false;
    memset(I.inv, 0, sizeof I.inv);
    memset(&I.stats, 0, sizeof I.stats);
    inv_items_refresh();
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!a->count) continue;
        if (a->mask & COMP_BIT(COMP_INVENTORY)) {
            CInventory* c = ecs_arch_column(a, COMP_INVENTORY);
            for (uint32_t i = 0; i < a->count; i++) c[i].handle = 0;
        }
        if ((a->mask & COMP_BIT(COMP_PICKUP)) && (a->mask & COMP_BIT(COMP_TRANSFORM))) {
            CPickup* p = ecs_arch_column(a, COMP_PICKUP);
            CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
            for (uint32_t i = 0; i < a->count; i++) { // (walked through: never a box the Characters stop at)
                p[i].ignore = 0, p[i].timer = 0.0f, p[i].phase = 0.0f, p[i].full = p[i].count ? p[i].count : 1;
                t[i].flags |= TRANSFORM_NO_COLLISION;
            }
        }
    }
    // the inventories made now (their starting items there for the first BeginPlay)
    const Entity* ents;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_INVENTORY))) continue;
        ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) get(w, ents[i]);
    }
}

void inv_end_play(void) {
    I.playing = false;
    I.w = NULL;
    I.ninv = I.nev = 0;
}

typedef struct Owner { Entity e; Inv* v; float p[3], r2; bool auto_take; } Owner;

void inv_tick(World* w, float dt) {
    if (!I.playing || w != I.w) return;
    double t0 = re_time_seconds();
    // the inventories: their actor gone, theirs too; where each is (for the Pickups)
    static Owner own[INV_MAX_INVENTORIES];
    uint32_t nown = 0, equipped = 0, live = 0;
    for (uint32_t k = 0; k < I.ninv; k++) {
        Inv* v = &I.inv[k];
        if (!v->owner) continue;
        if (!ecs_alive(w, v->owner) || !ecs_get(w, v->owner, COMP_INVENTORY)) {
            for (int s = 0; s < INV_EQ_COUNT; s++) unhang(w, v, s);
            v->owner = ENTITY_NONE;
            continue;
        }
        live++;
        for (int s = 0; s < INV_EQ_COUNT; s++) equipped += v->eq[s] != 0;
        const CInventory* c = ecs_get(w, v->owner, COMP_INVENTORY);
        const CTransform* t = ecs_get(w, v->owner, COMP_TRANSFORM);
        if (!t) continue;
        Owner* o = &own[nown++];
        o->e = v->owner, o->v = v;
        memcpy(o->p, t->location, 12);
        float r = c->pickup_radius > 0.0f ? c->pickup_radius : 120.0f;
        o->r2 = r * r, o->auto_take = c->auto_pickup != 0;
    }
    // the interact key (the possessed player's): its press this frame
    Entity player = bp_player(w);
    const CInventory* pc = ecs_alive(w, player) ? ecs_get(w, player, COMP_INVENTORY) : NULL;
    bool key = pc && pc->interact_key && bp_key_held((int)pc->interact_key), pressed = key && !I.key_was;
    I.key_was = key;
    // the Pickups: back after their time, spinning, touched; the nearest in reach of the interact key
    struct { Entity p; Inv* v; } want[MAX_TAKE];
    uint32_t nwant = 0, npick = 0;
    Entity near_p = ENTITY_NONE;
    float near_d = 1e30f;
    Inv* pv = NULL;
    for (uint32_t k = 0; k < nown; k++)
        if (own[k].e == player) pv = own[k].v;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_PICKUP) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || !a->count) continue;
        CPickup* pk = ecs_arch_column(a, COMP_PICKUP);
        CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            npick++;
            if (tr[i].flags & TRANSFORM_HIDDEN_GAME) {
                if (pk[i].timer > 0.0f && (pk[i].timer -= dt) <= 0.0f) {
                    pk[i].timer = 0.0f, pk[i].count = pk[i].full ? pk[i].full : 1;
                    tr[i].flags &= ~TRANSFORM_HIDDEN_GAME;
                    ecs_touch(w, ents[i]);
                }
                continue;
            }
            if (pk[i].spin) { // (the classic look: turning, bobbing)
                if (pk[i].phase == 0.0f) pk[i].base_z = tr[i].location[2], pk[i].phase = 0.001f + (float)(entity_index(ents[i]) % 97) * 0.065f;
                pk[i].phase += dt * 2.4f;
                if (pk[i].phase > 6283.0f) pk[i].phase -= 6283.0f;
                tr[i].rotation[1] += 90.0f * dt;
                if (tr[i].rotation[1] >= 180.0f) tr[i].rotation[1] -= 360.0f;
                tr[i].location[2] = pk[i].base_z + 6.0f * sinf(pk[i].phase);
                ecs_touch(w, ents[i]);
            }
            for (uint32_t k = 0; k < nown; k++) {
                const Owner* o = &own[k];
                float d[3] = { tr[i].location[0] - o->p[0], tr[i].location[1] - o->p[1], tr[i].location[2] - o->p[2] };
                float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], r2 = pk[i].radius > 0.0f ? pk[i].radius * pk[i].radius : o->r2;
                if (pk[i].ignore == o->e) { // (its dropper: once out of reach, it may take it again)
                    if (d2 > r2 * 1.44f) pk[i].ignore = 0;
                    continue;
                }
                if (d2 > r2) continue;
                if (pk[i].mode == PICKUP_ON_TOUCH && o->auto_take && nwant < MAX_TAKE) {
                    want[nwant].p = ents[i], want[nwant].v = o->v, nwant++;
                    break;
                }
                if (pk[i].mode == PICKUP_ON_INTERACT && o->e == player && pressed && d2 < near_d) near_d = d2, near_p = ents[i];
            }
        }
    }
    if (near_p && pv && nwant < MAX_TAKE) want[nwant].p = near_p, want[nwant].v = pv, nwant++;
    for (uint32_t k = 0; k < nwant; k++) // (after the walk: taking destroys)
        if (ecs_alive(w, want[k].p) && want[k].v->owner) take(w, want[k].v, want[k].p);
    // the events: the queued ones in order, then On Inventory Changed once per inventory (what they do may queue more)
    for (int pass = 0; pass < 4; pass++) {
        uint32_t n = I.nev;
        bool any = n > 0;
        static InvEv evs[MAX_EV];
        memcpy(evs, I.ev, n * sizeof evs[0]);
        I.nev = 0;
        for (uint32_t k = 0; k < n; k++) {
            if (!ecs_alive(w, evs[k].owner)) continue;
            const ItemDef* d = inv_item_by_hash(evs[k].item);
            BpValue args[2];
            memset(args, 0, sizeof args);
            bp_make_string(&args[0], d ? d->name : "None");
            bp_make_string(&args[1], evs[k].kind == BPK_EVENT_ITEM_USED ? "" : g_inv_equip_names[evs[k].slot % INV_EQ_COUNT]);
            bp_fire_actor_event(w, evs[k].owner, evs[k].kind, args, 2);
            I.stats.events++;
        }
        for (uint32_t k = 0; k < I.ninv; k++) {
            Inv* v = &I.inv[k];
            if (!v->owner || !v->changed) continue;
            v->changed = false, any = true;
            if (!ecs_alive(w, v->owner)) continue;
            bp_fire_actor_event(w, v->owner, BPK_EVENT_INV_CHANGED, NULL, 0);
            I.stats.events++;
        }
        if (!any) break;
    }
    I.stats.inventories = live, I.stats.pickups = npick, I.stats.equipped = equipped;
    I.stats.tick_ms = (re_time_seconds() - t0) * 1000.0;
}

const InvStats* inv_stats(void) { return &I.stats; }

// --- Saves: a record of the variables' stream (bp_vars_dump: name length, name, text flag, width, width x 16 bytes) ---
#define SAVE_NAME "#Inventory" // (no variable has a '#': bp_vars_load skips it)
#define SAVE_MAGIC 0x31564E49u // "INV1"

uint32_t inv_save_append(World* w, Entity e, uint8_t* buf, uint32_t len, uint32_t cap) {
    Inv* v = get(w, e);
    if (!v) return len;
    uint8_t rec[16 * 40];
    uint32_t n = 0;
    uint32_t hdr[2] = { SAVE_MAGIC, v->nslots };
    memcpy(rec, hdr, 8), n = 8;
    for (uint32_t i = 0; i < v->nslots; i++) memcpy(rec + n, &v->s[i], 8), n += 8;
    memcpy(rec + n, v->eq, sizeof v->eq), n += (uint32_t)sizeof v->eq;
    uint32_t wd = (n + 15) / 16;
    memset(rec + n, 0, wd * 16 - n);
    uint32_t nl = (uint32_t)strlen(SAVE_NAME), need = 1 + nl + 2 + wd * 16;
    if (len + need > cap || wd > 255) return len;
    buf[len++] = (uint8_t)nl;
    memcpy(buf + len, SAVE_NAME, nl), len += nl;
    buf[len++] = 0, buf[len++] = (uint8_t)wd;
    memcpy(buf + len, rec, wd * 16), len += wd * 16;
    return len;
}

bool inv_save_read(World* w, Entity e, const uint8_t* d, uint32_t len) {
    Inv* v = get(w, e);
    if (!v || len < 8 || memcmp(d, "RESAV001", 8)) return false;
    uint32_t at = 8, nl0 = (uint32_t)strlen(SAVE_NAME);
    while (at + 3 < len) { // (the records: the variables', then ours)
        uint32_t nl = d[at++];
        if (at + nl + 2 > len) return false;
        bool mine = nl == nl0 && !memcmp(d + at, SAVE_NAME, nl);
        at += nl;
        uint8_t txt = d[at++], wd = d[at++];
        uint32_t size;
        if (txt) {
            if (at + 2 > len) return false;
            size = 2u + (uint32_t)(d[at] | d[at + 1] << 8);
        } else size = wd * 16u;
        if (at + size > len) return false;
        if (mine && !txt && size >= 8) {
            uint32_t hdr[2];
            memcpy(hdr, d + at, 8);
            if (hdr[0] != SAVE_MAGIC || size < 8 + hdr[1] * 8 + sizeof v->eq) return false;
            memset(v->s, 0, sizeof v->s);
            uint32_t ns = hdr[1] < v->nslots ? hdr[1] : v->nslots;
            for (uint32_t i = 0; i < ns; i++) {
                memcpy(&v->s[i], d + at + 8 + i * 8, 8);
                if (!inv_item_by_hash(v->s[i].item) || v->s[i].count <= 0) v->s[i].item = 0, v->s[i].count = 0; // (an item gone since)
            }
            uint32_t eq[INV_EQ_COUNT];
            memcpy(eq, d + at + 8 + hdr[1] * 8, sizeof eq);
            for (int s = 0; s < INV_EQ_COUNT; s++) { // the equipment hung again
                unhang(w, v, s);
                const ItemDef* it = inv_item_by_hash(eq[s]);
                v->eq[s] = it ? eq[s] : 0;
                if (it) v->eq_ent[s] = hang(w, v->owner, it, s);
            }
            touched(v);
            return true;
        }
        at += size;
    }
    return false;
}

// --- WBP_Inventory: the ready-made widget (Content/UI), made when missing ---
uint32_t inv_widget_asset(void) {
    const char* vp = "/Content/UI/WBP_Inventory.rbp";
    BpAsset* a = bp_load(vp);
    if (a) return a->hash;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/UI", ui_content_root());
    re_make_dirs(dir);
    a = bp_create(BPA_CLASS, "/Content/UI", "WBP_Inventory", "User Widget");
    if (!a) return 0;
    if (!a->ui) a->ui = calloc(1, sizeof(UiTree)); // (an edit, not a frame)
    if (!a->ui) return 0;
    UiTree* t = a->ui;
    ui_tree_default(t);
    static const float dark[4] = { 0.035f, 0.035f, 0.045f, 0.92f };
    int panel = ui_tree_add(t, UIW_BORDER, 0);
    if (panel >= 0) {
        UiWidget* p = &t->w[panel];
        snprintf(p->name, UI_NAME, "Panel");
        p->anchor[0] = p->anchor[1] = p->anchor[2] = p->anchor[3] = 0.5f, p->align[0] = p->align[1] = 0.5f;
        p->pos[0] = p->pos[1] = 0.0f, p->size[0] = 820.0f, p->size[1] = 600.0f;
        memcpy(p->bg, dark, 16), p->radius = 12.0f, p->border_w = 1.0f;
        p->border_col[0] = p->border_col[1] = p->border_col[2] = 1.0f, p->border_col[3] = 0.12f;
    }
    static const struct { const char* name; uint8_t kind; float x, y, w, h; const char* text; float font; } k[] = {
        { "Title", UIW_TEXT, -385, -282, 500, 44, "Inventory", 32 },
        { "Slots", UIW_INVENTORY, -385, -225, 560, 470, "", 0 },
        { "Equipment", UIW_INVENTORY, 205, -225, 180, 470, "", 0 },
        { "Hint", UIW_TEXT, -385, 255, 770, 30, "Drag: move  -  right click: use  -  double click: equip  -  drag out: drop", 17 },
    };
    for (size_t i = 0; i < sizeof k / sizeof k[0]; i++) {
        int c = ui_tree_add(t, k[i].kind, 0);
        if (c < 0) break;
        UiWidget* x = &t->w[c];
        snprintf(x->name, UI_NAME, "%s", k[i].name);
        x->anchor[0] = x->anchor[1] = x->anchor[2] = x->anchor[3] = 0.5f, x->align[0] = x->align[1] = 0.0f;
        x->pos[0] = k[i].x, x->pos[1] = k[i].y, x->size[0] = k[i].w, x->size[1] = k[i].h;
        if (k[i].kind == UIW_TEXT) {
            snprintf(x->text, sizeof x->text, "%s", k[i].text);
            x->font_size = k[i].font, x->bold = i == 0;
            if (i == 3) x->color[0] = x->color[1] = x->color[2] = 0.62f;
        } else if (!strcmp(k[i].name, "Equipment")) x->columns = 1, x->orient = 1; // (orient 1: the equipment slots)
        else x->columns = 5;
    }
    bp_save(a);
    return a->hash;
}
