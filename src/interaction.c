// Interaction (interaction.h, notas-dev/interacao.md; módulo 5 do R7)
#include "interaction.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "debug_viewer.h"
#include "gameplay_tags.h"
#include "game_settings.h"
#include "i18n.h"
#include "input.h"
#include "inventory.h"
#include "platform.h"
#include "platform_files.h"
#include "plugins.h"
#include "render.h"
#include "ui_widget.h"

_Static_assert(sizeof(CInteractable) <= 128 && sizeof(CInteractor) <= 128 && sizeof(CDevice) <= 128, "a Blueprint class's defaults hold 128 bytes");

#define IA_CAND 4 // the best ones by score: the sight traced in order until one is seen
#define IA_DEG 0.017453292f

const char* const g_device_names[DEV_KIND_COUNT] = { "Door", "Treasure Chest", "Lever", "Button" };
const char* const g_device_motions[DEV_MOTION_COUNT] = { "Swing (Hinge)", "Slide", "Tilt" };

static struct {
    InteractHooks hooks;
    InteractStats st;
    bool playing;
    int test_pad; // -1: the real device
    Entity player;
    Entity hl[2];  // the highlight: the focused actor and its moving part
    bool prompt_made; // WBP_InteractPrompt on the screen this play
    uint32_t hl_mode;
    double profile_until;
    bool vr;              // (Q25) the player's view is a VR hand's ray, its use the hand's trigger (interaction_vr_pointer)
    bool vr_use;
    float vr_o[3], vr_d[3];
} G = { .test_pad = -1 };

void interaction_set_hooks(const InteractHooks* h) {
    if (h) G.hooks = *h;
    else memset(&G.hooks, 0, sizeof G.hooks);
}
void interaction_test_gamepad(int on) { G.test_pad = on; }
static bool gamepad(void) { return G.test_pad >= 0 ? G.test_pad != 0 : input_gamepad_used(); }

static void fire(World* w, Entity e, int ev, Entity a, float progress) {
    if (G.hooks.event && ecs_alive(w, e)) G.hooks.event(w, e, ev, a, progress);
}

// --- Geometry: an actor's box in the world, a ray against a box ---
static void world_box(World* w, Entity e, const CTransform* t, float mn[3], float mx[3]) {
    const CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    float lmn[3] = { -50, -50, -50 }, lmx[3] = { 50, 50, 50 };
    if (sm) {
        render_mesh_bounds(sm->mesh, lmn, lmx);
        if (!(lmx[0] > lmn[0]) && !(lmx[1] > lmn[1]) && !(lmx[2] > lmn[2])) // (not loaded yet: a basic shape's)
            for (int k = 0; k < 3; k++) lmn[k] = -50.0f, lmx[k] = 50.0f;
    }
    float rows[3][4];
    render_transform_rows(t, rows);
    for (int k = 0; k < 3; k++) {
        float c = rows[k][3], h = 0.0f;
        for (int j = 0; j < 3; j++) c += rows[k][j] * (lmn[j] + lmx[j]) * 0.5f, h += fabsf(rows[k][j]) * (lmx[j] - lmn[j]) * 0.5f;
        h = fmaxf(h, 2.0f);
        mn[k] = c - h, mx[k] = c + h;
    }
}
// Where the ray o + d s enters the box (s >= 0; inside: 0). False: it misses
static bool ray_box(const float o[3], const float d[3], const float mn[3], const float mx[3], float* s_in) {
    float t0 = 0.0f, t1 = 1e30f;
    for (int k = 0; k < 3; k++) {
        if (fabsf(d[k]) < 1e-8f) {
            if (o[k] < mn[k] || o[k] > mx[k]) return false;
            continue;
        }
        float inv = 1.0f / d[k], a = (mn[k] - o[k]) * inv, b = (mx[k] - o[k]) * inv;
        if (a > b) { float t = a; a = b; b = t; }
        t0 = fmaxf(t0, a), t1 = fminf(t1, b);
        if (t0 > t1) return false;
    }
    *s_in = t0;
    return true;
}
static float box_dist(const float p[3], const float mn[3], const float mx[3]) {
    float d2 = 0.0f;
    for (int k = 0; k < 3; k++) {
        float v = p[k] < mn[k] ? mn[k] - p[k] : p[k] > mx[k] ? p[k] - mx[k] : 0.0f;
        d2 += v * v;
    }
    return sqrtf(d2);
}

// An Interactor's view: where it looks from and toward; its pawn's eye (the reach and the sight are measured from it)
static bool view_of(World* w, Entity ie, Entity player, float o[3], float d[3], float eye[3]) {
    const CTransform* t = ecs_get(w, ie, COMP_TRANSFORM);
    if (!t) return false;
    const CCharacter* c = ecs_get(w, ie, COMP_CHARACTER);
    memcpy(eye, t->location, 12);
    eye[2] += c ? c->half_height * 0.75f : 50.0f;
    float yaw = c ? c->control_yaw : t->rotation[1], pitch = c ? c->control_pitch : t->rotation[0];
    if (ie == player && G.vr) { // (Q25) VR: the laser hand's ray, the reach measured from the hand
        memcpy(o, G.vr_o, 12), memcpy(d, G.vr_d, 12), memcpy(eye, G.vr_o, 12);
        return true;
    }
    if (ie == player && bp_player_camera(w, o, &yaw, &pitch)) {
    } else memcpy(o, eye, 12);
    float sp = sinf(pitch * IA_DEG), cp = cosf(pitch * IA_DEG), sy = sinf(yaw * IA_DEG), cy = cosf(yaw * IA_DEG);
    d[0] = cp * cy, d[1] = cp * sy, d[2] = sp;
    return true;
}

// --- The condition: an item tagged so in the instigator's Inventory, or the tag on it ---
static bool condition(World* w, const CInteractable* c, Entity who, bool consume) {
    if (!c->require[0] || (c->flags & IA_UNLOCKED)) return true;
    uint16_t src = c->flags & (IA_REQ_INVENTORY | IA_REQ_TAGS);
    if (!src) src = IA_REQ_INVENTORY | IA_REQ_TAGS;
    if ((src & IA_REQ_TAGS) && gt_has(w, who, c->require, false)) return true;
    uint32_t item = 0;
    int slot = -1;
    if ((src & IA_REQ_INVENTORY) && inv_find_tag(w, who, c->require, &item, &slot)) {
        if (consume && (c->flags & IA_CONSUME)) inv_remove(w, who, item, 1);
        return true;
    }
    return false;
}

static bool usable(const CInteractable* c) { return c && (c->flags & IA_ENABLED) && !((c->flags & IA_ONCE) && (c->flags & IA_USED)); }

bool interaction_can_interact(World* w, Entity interactor, Entity target) {
    const CInteractable* c = ecs_alive(w, target) ? ecs_get(w, target, COMP_INTERACTABLE) : NULL;
    return usable(c) && condition(w, c, interactor, false);
}

// --- Devices: the moving part's pose for t (0 closed .. 1 open) ---
static Entity part_of(World* w, Entity e, const CDevice* d) { return d->part && ecs_alive(w, d->part) ? d->part : e; }

static void pose(World* w, Entity e, CDevice* d) {
    Entity pe = part_of(w, e, d);
    CTransform* t = ecs_get(w, pe, COMP_TRANSFORM);
    if (!t) return;
    float u = d->t * d->t * (3.0f - 2.0f * d->t); // (eased)
    CTransform c = *t;
    memcpy(c.location, d->loc0, 12), memcpy(c.rotation, d->rot0, 12);
    if (d->motion == DEV_SLIDE) {
        float rows[3][4];
        render_transform_rows(&c, rows);
        float ax[3] = { rows[0][0], rows[1][0], rows[2][0] }, l = sqrtf(ax[0] * ax[0] + ax[1] * ax[1] + ax[2] * ax[2]);
        float s = u * d->amount * ((d->flags & DEV_HINGE_RIGHT) ? -1.0f : 1.0f) / (l > 1e-6f ? l : 1.0f);
        for (int k = 0; k < 3; k++) t->location[k] = d->loc0[k] + ax[k] * s;
        memcpy(t->rotation, d->rot0, 12);
    } else {
        // the pivot in the mesh's space: a door's hinge edge, a lid's back bottom edge, a lever's foot
        const CStaticMesh* sm = ecs_get(w, pe, COMP_STATIC_MESH);
        float mn[3] = { -50, -50, -50 }, mx[3] = { 50, 50, 50 };
        if (sm) {
            render_mesh_bounds(sm->mesh, mn, mx);
            if (!(mx[0] > mn[0])) for (int k = 0; k < 3; k++) mn[k] = -50.0f, mx[k] = 50.0f;
        }
        float p[3] = { (mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, (mn[2] + mx[2]) * 0.5f };
        if (d->motion == DEV_SWING) p[0] = (d->flags & DEV_HINGE_RIGHT) ? mx[0] : mn[0];
        else if (d->kind == DEV_LEVER) p[2] = mn[2];
        else p[0] = (d->flags & DEV_HINGE_RIGHT) ? mx[0] : mn[0], p[2] = mn[2];
        float rows[3][4], piv[3];
        render_transform_rows(&c, rows);
        for (int k = 0; k < 3; k++) piv[k] = rows[k][0] * p[0] + rows[k][1] * p[1] + rows[k][2] * p[2] + rows[k][3];
        float a = u * d->amount * (d->sign != 0.0f ? d->sign : 1.0f);
        if (d->motion == DEV_SWING) c.rotation[1] += a;
        else c.rotation[0] += a;
        memset(c.location, 0, 12);
        render_transform_rows(&c, rows);
        for (int k = 0; k < 3; k++) t->location[k] = piv[k] - (rows[k][0] * p[0] + rows[k][1] * p[1] + rows[k][2] * p[2]);
        memcpy(t->rotation, c.rotation, 12);
    }
    ecs_touch(w, pe);
}

static void device_start(World* w, Entity e, CDevice* d) {
    if (d->flags & DEV_STARTED) return;
    const CTransform* t = ecs_get(w, part_of(w, e, d), COMP_TRANSFORM);
    if (!t) return;
    memcpy(d->loc0, t->location, 12), memcpy(d->rot0, t->rotation, 12);
    d->flags |= DEV_STARTED;
    d->sign = 1.0f, d->timer = 0.0f;
    if (d->flags & DEV_START_ACTIVE) d->flags |= DEV_ACTIVE, d->t = 1.0f, pose(w, e, d);
    else d->flags &= ~DEV_ACTIVE, d->t = 0.0f;
}

bool interaction_device_set(World* w, Entity e, bool active, Entity instigator) {
    CDevice* d = ecs_alive(w, e) ? ecs_get(w, e, COMP_DEVICE) : NULL;
    if (!d) return false;
    device_start(w, e, d);
    if (((d->flags & DEV_ACTIVE) != 0) == active) return true;
    if (active && d->t <= 0.001f && d->motion == DEV_SWING && (d->flags & DEV_AWAY) && ecs_alive(w, instigator)) {
        // the way away from who opens it: its side of the door's plane (the part's Y) against the hinge's side
        const CTransform* it = ecs_get(w, instigator, COMP_TRANSFORM);
        CTransform c = { { 0 } };
        memcpy(c.rotation, d->rot0, 12), c.scale[0] = c.scale[1] = c.scale[2] = 1.0f;
        float rows[3][4];
        render_transform_rows(&c, rows);
        if (it) {
            float s = 0.0f;
            for (int k = 0; k < 3; k++) s += (it->location[k] - d->loc0[k]) * rows[k][1];
            float side = s >= 0.0f ? 1.0f : -1.0f;
            d->sign = (d->flags & DEV_HINGE_RIGHT) ? side : -side;
        }
    }
    d->flags = active ? d->flags | DEV_ACTIVE : d->flags & ~DEV_ACTIVE;
    d->timer = 0.0f;
    return true;
}
bool interaction_device_active(World* w, Entity e) {
    const CDevice* d = ecs_alive(w, e) ? ecs_get(w, e, COMP_DEVICE) : NULL;
    return d && (d->flags & DEV_ACTIVE);
}

// A Lever's / Button's target switched: its Custom Event (On), or a device toggled, a light on / off, an actor shown / hidden
static void switch_target(World* w, CDevice* d, bool on, Entity instigator) {
    Entity t = d->target;
    if (!t || !ecs_alive(w, t)) return;
    if (d->event[0]) {
        if (G.hooks.call) G.hooks.call(w, t, d->event, on);
        return;
    }
    if (ecs_get(w, t, COMP_DEVICE)) {
        interaction_device_set(w, t, !interaction_device_active(w, t), instigator);
        return;
    }
    CLight* l = ecs_get(w, t, COMP_LIGHT);
    if (l) {
        if (!l->ext) // (Unreal's options at their defaults, then Affects World toggled)
            l->ext = 1, l->affects_world = 1, l->temperature = 6500.0f, l->indirect_intensity = l->specular_scale = l->volumetric_scattering = 1.0f;
        l->affects_world = !l->affects_world;
        ecs_touch(w, t);
        return;
    }
    CTransform* tt = ecs_get(w, t, COMP_TRANSFORM);
    if (tt) tt->flags ^= TRANSFORM_HIDDEN_GAME, ecs_touch(w, t);
}

static void device_use(World* w, Entity e, CDevice* d, Entity who) {
    device_start(w, e, d);
    bool on = (d->flags & DEV_ACTIVE) != 0;
    switch (d->kind) {
    case DEV_CHEST:
        if (!on) // its items to the instigator's Inventory (what didn't fit stays for the next time)
            for (int k = 0; k < DEV_ITEMS; k++) {
                if (!d->item[k] || !d->count[k]) continue;
                int in = inv_add(w, who, d->item[k], (int)d->count[k]);
                d->count[k] -= (uint32_t)(in > 0 ? in : 0);
                d->flags |= DEV_GIVEN;
            }
        interaction_device_set(w, e, !on, who);
        break;
    case DEV_BUTTON:
        if (on) return; // (still in)
        interaction_device_set(w, e, true, who);
        switch_target(w, d, true, who);
        break;
    case DEV_LEVER:
        interaction_device_set(w, e, !on, who);
        switch_target(w, d, !on, who);
        break;
    default: interaction_device_set(w, e, !on, who); break;
    }
}

bool interaction_interact(World* w, Entity interactor, Entity target) {
    if (!w) return false;
    if (!target) target = interaction_focused(w, interactor);
    CInteractable* c = ecs_alive(w, target) ? ecs_get(w, target, COMP_INTERACTABLE) : NULL;
    if (!usable(c) || !condition(w, c, interactor, true)) {
        G.st.refused++;
        return false;
    }
    c->flags |= IA_USED | (c->require[0] ? IA_UNLOCKED : 0);
    G.st.interactions++;
    CDevice* d = ecs_get(w, target, COMP_DEVICE);
    if (d) device_use(w, target, d, interactor);
    fire(w, target, IA_EV_INTERACT, interactor, 1.0f);
    return true;
}

Entity interaction_focused(World* w, Entity interactor) {
    if (!interactor) interactor = G.player;
    const CInteractor* r = ecs_alive(w, interactor) ? ecs_get(w, interactor, COMP_INTERACTOR) : NULL;
    return r && r->focused && ecs_alive(w, r->focused) ? r->focused : ENTITY_NONE;
}

bool interaction_set_enabled(World* w, Entity target, bool enabled) {
    CInteractable* c = ecs_alive(w, target) ? ecs_get(w, target, COMP_INTERACTABLE) : NULL;
    if (!c) return false;
    c->flags = enabled ? c->flags | IA_ENABLED : c->flags & ~IA_ENABLED;
    if (enabled) c->flags &= ~IA_USED; // (enabled again: a used-once one may be used again)
    return true;
}

bool interaction_set_prompt(World* w, Entity target, const char* text) {
    CInteractable* c = ecs_alive(w, target) ? ecs_get(w, target, COMP_INTERACTABLE) : NULL;
    if (!c) return false;
    snprintf(c->prompt, sizeof c->prompt, "%s", text ? text : "");
    return true;
}

// --- The prompt's text and key ---
static const char* verb(World* w, Entity e) {
    const CDevice* d = ecs_get(w, e, COMP_DEVICE);
    if (!d) return "Interact";
    bool on = (d->flags & DEV_ACTIVE) != 0;
    switch (d->kind) {
    case DEV_DOOR: case DEV_CHEST: return on ? "Close" : "Open";
    case DEV_LEVER: return "Pull";
    case DEV_BUTTON: return "Press";
    default: return "Interact";
    }
}
static void prompt_text(World* w, Entity e, const CInteractable* c, char* out, uint32_t cap) {
    const CName* nm = ecs_get(w, e, COMP_NAME);
    const char* name = c->name[0] ? c->name : nm ? nm->text : "";
    if (!c->prompt[0]) {
        if (c->name[0]) snprintf(out, cap, "%s %s", TR(verb(w, e)), c->name);
        else snprintf(out, cap, "%s", TR(verb(w, e)));
        return;
    }
    uint32_t n = 0;
    for (const char* p = c->prompt; *p && n + 1 < cap; p++) { // {0}: the name
        if (p[0] == '{' && p[1] == '0' && p[2] == '}') {
            for (const char* q = name; *q && n + 1 < cap; q++) out[n++] = *q;
            p += 2;
        } else out[n++] = *p;
    }
    out[n] = 0;
}
static const char* short_key(const char* k) { // a gamepad button's usual label (Xbox's)
    static const struct { const char* n; const char* s; } m[] = {
        { "Gamepad Face Button Bottom", "A" }, { "Gamepad Face Button Right", "B" }, { "Gamepad Face Button Left", "X" },
        { "Gamepad Face Button Top", "Y" }, { "Gamepad Left Shoulder", "LB" }, { "Gamepad Right Shoulder", "RB" },
        { "Gamepad Left Trigger Axis", "LT" }, { "Gamepad Right Trigger Axis", "RT" }, { "Gamepad Special Right", "Start" },
        { "Gamepad Special Left", "Back" }, { "Gamepad D-pad Up", "D-pad Up" }, { "Gamepad D-pad Down", "D-pad Down" },
        { "Left Mouse Button", "LMB" }, { "Right Mouse Button", "RMB" },
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++)
        if (!_stricmp(k, m[i].n)) return m[i].s;
    return k;
}
static void key_label(const CInteractable* c, const CInteractor* r, char* out, uint32_t cap) {
    const char* action = c && c->action[0] ? c->action : r ? r->action : "";
    bool pad = gamepad();
    if (action[0]) {
        const char* k = gus_action_key(action, pad);
        if (k && k[0]) {
            snprintf(out, cap, "%s", short_key(k));
            return;
        }
    }
    if (pad) {
        snprintf(out, cap, "X");
        return;
    }
    uint32_t key = c && c->key ? c->key : r ? r->key : 'E';
    if (key >= 32 && key < 97) snprintf(out, cap, "%c", (char)key);
    else if (key == 257) snprintf(out, cap, "Enter");
    else if (key >= 290 && key <= 301) snprintf(out, cap, "F%u", key - 289);
    else snprintf(out, cap, "?");
}

bool interaction_prompt(World* w, Entity interactor, char* text, uint32_t tcap, char* key, uint32_t kcap, float* progress, bool* can) {
    if (!interactor) interactor = G.player;
    const CInteractor* r = ecs_alive(w, interactor) ? ecs_get(w, interactor, COMP_INTERACTOR) : NULL;
    Entity f = interaction_focused(w, interactor);
    const CInteractable* c = f ? ecs_get(w, f, COMP_INTERACTABLE) : NULL;
    if (text && tcap) text[0] = 0;
    if (key && kcap) key[0] = 0;
    if (progress) *progress = 0.0f;
    if (can) *can = false;
    if (!r || !c) return false;
    bool ok = condition(w, c, interactor, false);
    if (can) *can = ok;
    if (progress) *progress = r->progress;
    if (text && tcap) {
        if (ok) prompt_text(w, f, c, text, tcap);
        else { // "Locked (Red Key)": the item's name with the tag, else the tag
            const char* what = c->require;
            for (uint32_t i = 0; i < inv_item_count(); i++)
                if (inv_item_has_tag(inv_item_at(i), c->require)) {
                    what = inv_item_at(i)->display;
                    break;
                }
            snprintf(text, tcap, "%s (%s)", TR("Locked"), what);
        }
    }
    if (key && kcap) key_label(c, r, key, kcap);
    return true;
}

// --- Play ---
void interaction_begin_play(World* w) {
    memset(&G.st, 0, sizeof G.st);
    G.playing = true, G.player = ENTITY_NONE, G.hl[0] = G.hl[1] = ENTITY_NONE, G.hl_mode = 0, G.prompt_made = false;
    // the devices' moving parts and the switched actors Movable (their collision boxes follow them), their poses read;
    // the run's flags of the Interactables cleared (a level saved after a play keeps none)
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count) continue;
        const Entity* ents = ecs_arch_entities(a);
        if (a->mask & COMP_BIT(COMP_INTERACTABLE)) {
            CInteractable* c = ecs_arch_column(a, COMP_INTERACTABLE);
            for (uint32_t i = 0; i < a->count; i++) c[i].flags &= ~(IA_USED | IA_UNLOCKED);
        }
        if (a->mask & COMP_BIT(COMP_INTERACTOR)) {
            CInteractor* r = ecs_arch_column(a, COMP_INTERACTOR);
            for (uint32_t i = 0; i < a->count; i++) r[i].focused = r[i].held = r[i].hold_on = 0, r[i].hold = r[i].progress = 0.0f;
        }
        if (!(a->mask & COMP_BIT(COMP_DEVICE))) continue;
        CDevice* d = ecs_arch_column(a, COMP_DEVICE);
        for (uint32_t i = 0; i < a->count; i++) {
            d[i].flags &= ~(DEV_STARTED | DEV_ACTIVE | DEV_GIVEN);
            Entity moving[2] = { part_of(w, ents[i], &d[i]), d[i].target };
            for (int k = 0; k < 2; k++) {
                CTransform* t = moving[k] && ecs_alive(w, moving[k]) ? ecs_get(w, moving[k], COMP_TRANSFORM) : NULL;
                if (t && !ecs_get(w, moving[k], COMP_LIGHT)) t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
            }
        }
    }
    // (after: a moving part may be another device's)
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_DEVICE)) || !a->count) continue;
        CDevice* d = ecs_arch_column(a, COMP_DEVICE);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) device_start(w, ents[i], &d[i]);
    }
}

// The possessed pawn gets an Interactor (when play starts, and when another is possessed), the prompt once a play
static void player_ready(World* w, Entity player) {
    if (!plugins_gameplay_enabled()) return; // (the Gameplay Framework plugin off: no Interactor, no prompt)
    if (!player || !ecs_alive(w, player)) return;
    if (!ecs_get(w, player, COMP_INTERACTOR) && ecs_add(w, player, COMP_INTERACTOR))
        ecs_component_defaults(COMP_INTERACTOR, ecs_get(w, player, COMP_INTERACTOR));
    const CInteractor* r = ecs_get(w, player, COMP_INTERACTOR);
    if (r && (r->flags & IAR_PROMPT) && !G.prompt_made) { // the prompt on the screen (the example widget)
        G.prompt_made = true;
        BpAsset* a = bp_find_hash(interaction_prompt_asset());
        Entity we = a && (a->compiled || bp_compile(a)) ? ui_create_widget(w, a) : ENTITY_NONE;
        if (we) ui_add_to_viewport(we, 5);
    }
}

void interaction_after_begin_play(World* w, Entity player) {
    G.player = player;
    player_ready(w, player);
}

void interaction_end_play(World* w) {
    (void)w;
    G.playing = false;
    G.player = ENTITY_NONE, G.hl[0] = G.hl[1] = ENTITY_NONE;
    memset(&G.hooks, 0, sizeof G.hooks);
}

static bool key_down(const CInteractable* c, const CInteractor* r) {
    const char* action = c && c->action[0] ? c->action : r->action;
    if (action[0]) {
        float v[3];
        if (input_action_state(action, v, NULL, NULL, NULL)) return fabsf(v[0]) > 0.5f;
    }
    uint32_t key = c && c->key ? c->key : r->key;
    if (key && G.hooks.key && G.hooks.key((int)key)) return true;
    return !action[0] && input_key_held("Gamepad Face Button Left"); // (no action: a gamepad's X)
}

typedef struct Cand { Entity e; float score, mn[3], mx[3]; } Cand;

// The pawn's eye sees the box: a trace to its nearest point arrives there (its own surface stops it: within 8 cm). A
// device's moving part counts as it (a chest's lid covers its box)
static bool sees_box(const float eye[3], const float mn[3], const float mx[3]) {
    float q[3], len = 0.0f;
    for (int j = 0; j < 3; j++) q[j] = fminf(fmaxf(eye[j], mn[j]), mx[j]), len += (q[j] - eye[j]) * (q[j] - eye[j]);
    len = sqrtf(len);
    if (len < 8.0f) return true; // (inside it, or touching)
    G.st.sight_traces++;
    return bp_trace_boxes(eye, q, 1.0f) * len >= len - 8.0f;
}
static bool sees(World* w, const float eye[3], Entity e, const float mn[3], const float mx[3]) {
    if (sees_box(eye, mn, mx)) return true;
    const CDevice* d = ecs_get(w, e, COMP_DEVICE);
    const CTransform* pt = d && d->part && ecs_alive(w, d->part) ? ecs_get(w, d->part, COMP_TRANSFORM) : NULL;
    if (!pt) return false;
    float pmn[3], pmx[3];
    world_box(w, d->part, pt, pmn, pmx);
    return sees_box(eye, pmn, pmx);
}

// One Interactor's focus: every Interactable in reach and in its view cone (or under its ray), the best seen
static Entity find_focus(World* w, Entity ie, const CInteractor* r, Entity player) {
    float o[3], d[3], eye[3];
    if (!view_of(w, ie, player, o, d, eye)) return ENTITY_NONE;
    Cand best[IA_CAND];
    uint32_t nb = 0;
    float cam_back = 0.0f; // (a camera behind the pawn: its ray reaches the reach past the pawn)
    for (int k = 0; k < 3; k++) cam_back += (eye[k] - o[k]) * d[k];
    cam_back = fmaxf(cam_back, 0.0f);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_INTERACTABLE) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || !a->count) continue;
        const CInteractable* c = ecs_arch_column(a, COMP_INTERACTABLE);
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            if (!usable(&c[i]) || ents[i] == ie || (t[i].flags & TRANSFORM_HIDDEN_GAME)) continue;
            float reach = c[i].range > 0.0f ? c[i].range : r->range;
            // (a quick reject by the location first: most are far)
            float q[3] = { t[i].location[0] - eye[0], t[i].location[1] - eye[1], t[i].location[2] - eye[2] };
            float far = reach + 300.0f * fmaxf(fmaxf(t[i].scale[0], t[i].scale[1]), t[i].scale[2]);
            if (q[0] * q[0] + q[1] * q[1] + q[2] * q[2] > far * far) continue;
            Cand x;
            world_box(w, ents[i], &t[i], x.mn, x.mx);
            float dist = box_dist(eye, x.mn, x.mx);
            if (dist > reach) continue;
            float cen[3], to[3], len = 0.0f;
            for (int k = 0; k < 3; k++) cen[k] = (x.mn[k] + x.mx[k]) * 0.5f, to[k] = cen[k] - o[k], len += to[k] * to[k];
            len = sqrtf(len);
            float cosv = len > 1e-3f ? (to[0] * d[0] + to[1] * d[1] + to[2] * d[2]) / len : 1.0f;
            float s_in = 0.0f;
            bool aimed = ray_box(o, d, x.mn, x.mx, &s_in) && s_in <= cam_back + reach;
            float lim = cosf((c[i].angle > 0.0f ? c[i].angle : r->angle) * IA_DEG);
            if (!aimed && cosv < lim) continue;
            G.st.candidates++;
            // the score: its priority, the ray on it, how centered, how near
            float centered = lim < 0.9999f ? fminf(fmaxf((cosv - lim) / (1.0f - lim), 0.0f), 1.0f) : 1.0f;
            x.e = ents[i];
            x.score = (float)c[i].priority * 1000.0f + (aimed ? 200.0f : 0.0f) + centered * 100.0f + (1.0f - dist / fmaxf(reach, 1.0f)) * 50.0f;
            uint32_t at = nb < IA_CAND ? nb++ : IA_CAND;
            if (at == IA_CAND) {
                if (x.score <= best[IA_CAND - 1].score) continue;
                at = IA_CAND - 1;
            }
            while (at > 0 && best[at - 1].score < x.score) best[at] = best[at - 1], at--;
            best[at] = x;
        }
    }
    for (uint32_t k = 0; k < nb; k++) { // the best one the pawn sees (a wall between: the next)
        if (!(r->flags & IAR_SIGHT) || sees(w, eye, best[k].e, best[k].mn, best[k].mx)) return best[k].e;
    }
    return ENTITY_NONE;
}

static void hold_reset(World* w, CInteractor* r, Entity ie) {
    if (r->hold > 0.0f && r->hold_on) fire(w, r->hold_on, IA_EV_HOLD, ie, 0.0f); // (let go: the progress back to 0)
    r->hold = r->progress = 0.0f, r->hold_on = 0;
}

void interaction_tick(World* w, float dt, Entity player) {
    if (!G.playing) return;
    double t0 = re_time_seconds();
    if (player && player != G.player) player_ready(w, player); // (another pawn possessed: its Interactor)
    G.player = player;
    G.st.candidates = G.st.sight_traces = 0;
    uint32_t nint = 0, nact = 0, ndev = 0;
    // the devices: toward open / closed, the auto close
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (a->mask & COMP_BIT(COMP_INTERACTABLE)) nint += a->count;
        if (!(a->mask & COMP_BIT(COMP_DEVICE)) || !a->count) continue;
        CDevice* d = ecs_arch_column(a, COMP_DEVICE);
        const Entity* ents = ecs_arch_entities(a);
        ndev += a->count;
        for (uint32_t i = 0; i < a->count; i++) {
            CDevice* x = &d[i];
            if (!(x->flags & DEV_STARTED)) device_start(w, ents[i], x);
            bool on = (x->flags & DEV_ACTIVE) != 0;
            if (on && x->auto_close > 0.0f && x->t >= 1.0f && (x->timer += dt) >= x->auto_close) x->flags &= ~DEV_ACTIVE, x->timer = 0.0f, on = false;
            float goal = on ? 1.0f : 0.0f;
            if (x->t == goal) continue;
            float step = x->time > 0.001f ? dt / x->time : 1.0f;
            x->t = on ? fminf(x->t + step, 1.0f) : fmaxf(x->t - step, 0.0f);
            pose(w, ents[i], x);
        }
    }
    // the Interactors: the focus, its events, the key and the hold
    G.hl[0] = G.hl[1] = ENTITY_NONE, G.hl_mode = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_INTERACTOR)) || !a->count) continue;
        nact += a->count;
        for (uint32_t i = 0; i < a->count; i++) {
            Entity ie = ecs_arch_entities(a)[i];
            CInteractor* r = &((CInteractor*)ecs_arch_column(a, COMP_INTERACTOR))[i];
            Entity f = find_focus(w, ie, r, player);
            Entity old = r->focused && ecs_alive(w, r->focused) ? r->focused : ENTITY_NONE;
            if (f != old) { // (the events may run game code: the component read again after)
                hold_reset(w, r, ie);
                r->focused = f;
                G.st.focus_changes++;
                if (old) fire(w, old, IA_EV_FOCUS_END, ie, 0.0f);
                if (f) fire(w, f, IA_EV_FOCUS_BEGIN, ie, 0.0f);
                if (!ecs_alive(w, ie) || !(r = ecs_get(w, ie, COMP_INTERACTOR))) break;
                f = r->focused;
            }
            const CInteractable* c = f ? ecs_get(w, f, COMP_INTERACTABLE) : NULL;
            bool down = ie == player && (key_down(c, r) || (G.vr && G.vr_use)); // (only the player's keys: another Interactor uses the Interact node)
            bool pressed = down && !r->held;
            r->held = down;
            if (c && usable(c)) {
                if (c->hold_time <= 0.0f) {
                    if (pressed) interaction_interact(w, ie, f);
                } else if (down && (pressed || r->hold_on == f)) {
                    r->hold_on = f;
                    r->hold += dt;
                    r->progress = fminf(r->hold / c->hold_time, 1.0f);
                    float p = r->progress;
                    bool done = r->hold >= c->hold_time;
                    fire(w, f, IA_EV_HOLD, ie, p);
                    if ((r = ecs_get(w, ie, COMP_INTERACTOR)) == NULL) break;
                    if (done) {
                        r->hold = r->progress = 0.0f, r->hold_on = 0; // (held on: it waits for a new press)
                        interaction_interact(w, ie, f);
                        if ((r = ecs_get(w, ie, COMP_INTERACTOR)) == NULL) break;
                    }
                } else if (!down && r->hold_on) hold_reset(w, r, ie);
            } else if (r->hold_on) hold_reset(w, r, ie);
            if (ie == player && r->focused) G.hl[0] = r->focused, G.hl_mode = r->flags & (IAR_OUTLINE | IAR_GLOW);
            a = &w->archetypes[ai]; // (the events may have moved the rows: a fresh look)
            if (i >= a->count) break;
        }
    }
    if (G.hl[0]) {
        const CDevice* d = ecs_get(w, G.hl[0], COMP_DEVICE);
        if (d && d->part && ecs_alive(w, d->part)) G.hl[1] = d->part;
    }
    G.st.interactables = nint, G.st.interactors = nact, G.st.devices = ndev;
    G.st.tick_ms = (re_time_seconds() - t0) * 1000.0;
}

void interaction_vr_pointer(bool on, const float o[3], const float d[3], bool use) {
    G.vr = on, G.vr_use = on && use;
    if (on) memcpy(G.vr_o, o, 12), memcpy(G.vr_d, d, 12);
}

Entity interaction_highlight(World* w, uint32_t* mode) {
    if (mode) *mode = G.hl_mode;
    return G.playing && G.hl_mode && G.hl[0] && ecs_alive(w, G.hl[0]) ? G.hl[0] : ENTITY_NONE;
}
Entity interaction_highlight_part(World* w) { return G.playing && G.hl_mode && G.hl[1] && ecs_alive(w, G.hl[1]) ? G.hl[1] : ENTITY_NONE; }

const InteractStats* interaction_stats(void) { return &G.st; }

// --- The ready-made actors ---
static void set_mesh(World* w, Entity e, float r, float g, float b, float rough) {
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    if (!sm) return;
    sm->mesh = SHAPE_CUBE;
    sm->base_color[0] = r, sm->base_color[1] = g, sm->base_color[2] = b;
    sm->roughness = rough;
    sm->cast_shadow = 1;
}

void interaction_setup_device(World* w, Entity e, int kind) {
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    CDevice* d = ecs_get(w, e, COMP_DEVICE);
    CInteractable* c = ecs_get(w, e, COMP_INTERACTABLE);
    if (!t || !d || !c) return;
    d->kind = (uint32_t)(kind >= 0 && kind < DEV_KIND_COUNT ? kind : DEV_DOOR);
    t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
    float ground = t->location[2] - 100.0f; // (Place Actors puts it 1 m up: on the floor)
    if (ground < -50.0f || ground > 50.0f) ground = 0.0f;
    switch (d->kind) {
    case DEV_DOOR: // 1 x 2.1 m, 10 cm thick, swinging 95 degrees away from who opens it
        t->scale[0] = 1.0f, t->scale[1] = 0.1f, t->scale[2] = 2.1f, t->location[2] = ground + 105.0f;
        d->motion = DEV_SWING, d->amount = 95.0f, d->time = 0.6f, d->flags = DEV_AWAY;
        set_mesh(w, e, 0.42f, 0.26f, 0.14f, 0.6f);
        break;
    case DEV_CHEST: { // a 90 x 60 x 50 cm box and its lid (a second actor: the moving part), opening 100 degrees
        t->scale[0] = 0.6f, t->scale[1] = 0.9f, t->scale[2] = 0.5f, t->location[2] = ground + 25.0f;
        d->motion = DEV_TILT, d->amount = 100.0f, d->time = 0.7f, d->flags = 0;
        set_mesh(w, e, 0.45f, 0.29f, 0.13f, 0.7f);
        Entity lid = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
        CTransform* lt = lid ? ecs_get(w, lid, COMP_TRANSFORM) : NULL;
        t = ecs_get(w, e, COMP_TRANSFORM), d = ecs_get(w, e, COMP_DEVICE); // (the create may have grown the columns)
        if (lt && t && d) {
            memcpy(lt->location, t->location, 12), memcpy(lt->rotation, t->rotation, 12);
            lt->location[2] = ground + 55.0f;
            lt->scale[0] = 0.62f, lt->scale[1] = 0.92f, lt->scale[2] = 0.1f;
            lt->flags = MOBILITY_MOVABLE;
            set_mesh(w, lid, 0.36f, 0.22f, 0.1f, 0.7f);
            const CName* nm = ecs_get(w, e, COMP_NAME);
            snprintf(((CName*)ecs_get(w, lid, COMP_NAME))->text, sizeof(((CName*)0)->text), "%s_Lid", nm ? nm->text : "Chest");
            d->part = lid;
        }
        break;
    }
    case DEV_LEVER: // a 60 cm handle on the floor, tilted back; pulled it tilts forward (70 degrees)
        t->scale[0] = 0.06f, t->scale[1] = 0.06f, t->scale[2] = 0.6f, t->location[2] = ground + 30.0f;
        d->motion = DEV_TILT, d->amount = 70.0f, d->time = 0.3f, d->flags = 0;
        set_mesh(w, e, 0.2f, 0.2f, 0.22f, 0.35f);
        t->rotation[0] = -35.0f;
        { // (its foot stays where it is: the tilt about the foot)
            float rows[3][4];
            CTransform u = *t;
            memset(u.location, 0, 12);
            render_transform_rows(&u, rows);
            for (int k = 0; k < 3; k++) t->location[k] += rows[k][2] * 50.0f - (k == 2 ? 30.0f : 0.0f);
        }
        break;
    case DEV_BUTTON: // a red button on a wall, 1.1 m up: pressed it goes in 4 cm for 0.4 s
        t->scale[0] = 0.06f, t->scale[1] = 0.16f, t->scale[2] = 0.16f, t->location[2] = ground + 110.0f;
        d->motion = DEV_SLIDE, d->amount = 4.0f, d->time = 0.08f, d->auto_close = 0.4f, d->flags = DEV_HINGE_RIGHT;
        set_mesh(w, e, 0.75f, 0.06f, 0.05f, 0.4f);
        break;
    default: break;
    }
    c = ecs_get(w, e, COMP_INTERACTABLE); // (a chest's lid was created meanwhile)
    if (c) c->flags = IA_ENABLED | IA_REQ_INVENTORY | IA_REQ_TAGS;
    ecs_touch(w, e);
}

// --- WBP_InteractPrompt: the example widget ---
static int find_pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    return -1;
}
static bool wire(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    int pa = find_pin(g, a, 1, ap), pb = find_pin(g, b, 0, bpn);
    return pa >= 0 && pb >= 0 && bp_link(g, a, (uint8_t)pa, b, (uint8_t)pb);
}
static uint32_t add_node(BpAsset* a, BpGraph* g, uint16_t kind, float x, float y, const char* text) {
    BpNode* n = bp_add_node(a, g, kind, x, y, text, NULL);
    return n ? n->id : 0;
}

uint32_t interaction_prompt_asset(void) {
    const char* vp = "/Content/UI/WBP_InteractPrompt.rbp";
    BpAsset* a = bp_load(vp);
    if (a) return a->hash;
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/UI", ui_content_root());
    re_make_dirs(dir);
    a = bp_create(BPA_CLASS, "/Content/UI", "WBP_InteractPrompt", "User Widget");
    if (!a) return 0;
    if (!a->ui) a->ui = calloc(1, sizeof(UiTree));
    if (!a->ui) return 0;
    UiTree* t = a->ui;
    ui_tree_default(t);
    // a frame below the screen's middle: the key in a light box, the text, the hold's bar under it
    int fr = ui_tree_add(t, UIW_CANVAS, 0);
    if (fr < 0) return 0;
    UiWidget* f = &t->w[fr];
    snprintf(f->name, UI_NAME, "Frame");
    f->anchor[0] = f->anchor[2] = 0.5f, f->anchor[1] = f->anchor[3] = 0.74f;
    f->align[0] = f->align[1] = 0.5f;
    f->pos[0] = f->pos[1] = 0.0f, f->size[0] = 460.0f, f->size[1] = 64.0f;
    static const struct { uint8_t kind; const char* name; float x, y, w, h, ax, ay; float color[4], bg[4]; float font; } wd[] = {
        { UIW_BORDER, "Back", 0, 0, 460, 64, 0, 0, { 1, 1, 1, 1 }, { 0.02f, 0.02f, 0.025f, 0.62f }, 0 },
        { UIW_BORDER, "KeyBox", 10, 10, 44, 44, 0, 0, { 1, 1, 1, 1 }, { 0.93f, 0.93f, 0.9f, 0.96f }, 0 },
        { UIW_TEXT, "KeyText", 32, 32, 44, 44, 0.5f, 0.5f, { 0.05f, 0.05f, 0.06f, 1 }, { 0, 0, 0, 0 }, 22 },
        { UIW_TEXT, "PromptText", 68, 26, 380, 30, 0, 0.5f, { 1, 1, 1, 1 }, { 0, 0, 0, 0 }, 22 },
        { UIW_PROGRESS, "HoldBar", 68, 46, 380, 7, 0, 0, { 1.0f, 0.78f, 0.3f, 1 }, { 0.1f, 0.1f, 0.1f, 0.85f }, 0 },
    };
    for (size_t i = 0; i < sizeof wd / sizeof wd[0]; i++) {
        int k = ui_tree_add(t, wd[i].kind, fr);
        if (k < 0) return 0;
        UiWidget* u = &t->w[k];
        snprintf(u->name, UI_NAME, "%s", wd[i].name);
        u->anchor[0] = u->anchor[2] = 0.0f, u->anchor[1] = u->anchor[3] = 0.0f;
        u->align[0] = wd[i].ax, u->align[1] = wd[i].ay;
        u->pos[0] = wd[i].x, u->pos[1] = wd[i].y, u->size[0] = wd[i].w, u->size[1] = wd[i].h;
        memcpy(u->color, wd[i].color, sizeof u->color);
        memcpy(u->bg, wd[i].bg, sizeof u->bg);
        u->radius = wd[i].kind == UIW_TEXT ? 0.0f : wd[i].kind == UIW_PROGRESS ? 3.0f : 7.0f;
        if (wd[i].kind == UIW_PROGRESS) u->percent = 0.0f;
        if (wd[i].kind == UIW_TEXT) {
            snprintf(u->text, sizeof u->text, "%s", !strcmp(wd[i].name, "KeyText") ? "E" : "Open");
            u->font_size = wd[i].font, u->bold = 1, u->shadow = !strcmp(wd[i].name, "PromptText");
        }
    }
    // Event Tick: the player's Interactor's prompt into the widgets; the frame only while something is focused, the bar
    // only while it is held
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t tick = add_node(a, g, BPK_EVENT_TICK, 0, 0, NULL);
    uint32_t pl = add_node(a, g, BPK_GET_PLAYER, 0, 220, NULL);
    uint32_t gp = add_node(a, g, BPK_IA_GET_PROMPT, 260, 220, NULL);
    uint32_t has = add_node(a, g, BPK_GREATER, 560, 420, NULL);
    uint32_t v1 = add_node(a, g, BPK_WIDGET_SET_VISIBLE, 320, 0, "Frame");
    uint32_t s1 = add_node(a, g, BPK_WIDGET_SET_TEXT, 620, 0, "PromptText");
    uint32_t s2 = add_node(a, g, BPK_WIDGET_SET_TEXT, 920, 0, "KeyText");
    uint32_t s3 = add_node(a, g, BPK_WIDGET_SET_PERCENT, 1220, 0, "HoldBar");
    uint32_t v2 = add_node(a, g, BPK_WIDGET_SET_VISIBLE, 1520, 0, "HoldBar");
    bool ok = tick && pl && gp && has && v1 && s1 && s2 && s3 && v2;
    ok = ok && wire(g, tick, "", v1, "") && wire(g, v1, "", s1, "") && wire(g, s1, "", s2, "") && wire(g, s2, "", s3, "") && wire(g, s3, "", v2, "");
    ok = ok && wire(g, pl, "Player", gp, "Target") && wire(g, gp, "Has Focus", v1, "Visible") && wire(g, gp, "Prompt", s1, "In Text") &&
         wire(g, gp, "Key", s2, "In Text") && wire(g, gp, "Hold Progress", s3, "In Percent") && wire(g, gp, "Hold Progress", has, "A") &&
         wire(g, has, "", v2, "Visible");
    if (!ok) printf("[RealEngine] WBP_InteractPrompt: its graph could not be made\n");
    bp_compile(a);
    if (!bp_save(a)) return 0;
    return a->hash;
}

// --- The Debug Viewer: the walk's cost, each Interactable ---
static void collect(DvList* l, const DvContext* ctx) {
    l->gpu = false, l->estimated = true;
    l->ms = G.st.tick_ms;
    snprintf(l->note, sizeof l->note, "%u Interactables, %u Interactors, %u devices; last frame %u in reach, %u sight traces; since play %u used, %u refused, %u focus changes",
             G.st.interactables, G.st.interactors, G.st.devices, G.st.candidates, G.st.sight_traces, G.st.interactions, G.st.refused, G.st.focus_changes);
    dv_add(l, "Focus, keys, devices", "the whole walk", (float)G.st.tick_ms, ENTITY_NONE);
    World* w = ctx ? ctx->world : NULL;
    if (!w || !G.st.interactables) return;
    float each = (float)(G.st.tick_ms / (double)G.st.interactables);
    Entity focus = interaction_focused(w, G.player);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_INTERACTABLE)) || !a->count) continue;
        const CInteractable* c = ecs_arch_column(a, COMP_INTERACTABLE);
        const CName* nm = (a->mask & COMP_BIT(COMP_NAME)) ? ecs_arch_column(a, COMP_NAME) : NULL;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            char info[64];
            snprintf(info, sizeof info, "%s%s%s%s", ents[i] == focus ? "focused" : (c[i].flags & IA_ENABLED) ? "enabled" : "disabled",
                     (c[i].flags & IA_USED) ? ", used" : "", c[i].require[0] ? ", needs " : "", c[i].require);
            dv_add(l, nm ? nm[i].text : "Actor", info, each, ents[i]);
        }
    }
}
void interaction_register_debug(void) { dv_register("Interaction", collect); }

// --- The self-test ---
static struct { uint32_t n[IA_EV_COUNT]; Entity last[IA_EV_COUNT]; float progress; } g_rec;
static bool g_keys[512];
static void rec_event(World* w, Entity e, int ev, Entity a, float progress) {
    (void)w, (void)a;
    g_rec.n[ev]++, g_rec.last[ev] = e;
    if (ev == IA_EV_HOLD) g_rec.progress = progress;
}
static struct { uint32_t n; char name[IA_ACTION]; bool on; } g_call;
static bool rec_call(World* w, Entity e, const char* event, bool on) {
    (void)w, (void)e;
    g_call.n++, g_call.on = on;
    snprintf(g_call.name, sizeof g_call.name, "%s", event);
    return true;
}
static bool test_key(int key) { return key >= 0 && key < 512 && g_keys[key]; }

static Entity cube(World* w, float x, float y, float z, uint64_t extra) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | extra);
    if (!e) return e;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z;
    t->scale[0] = t->scale[1] = t->scale[2] = 0.5f;
    ((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->mesh = SHAPE_CUBE;
    if (extra & COMP_BIT(COMP_INTERACTABLE)) ecs_component_defaults(COMP_INTERACTABLE, ecs_get(w, e, COMP_INTERACTABLE));
    if (extra & COMP_BIT(COMP_DEVICE)) ecs_component_defaults(COMP_DEVICE, ecs_get(w, e, COMP_DEVICE));
    return e;
}

static void frames(World* w, int n, float dt, Entity player) {
    for (int i = 0; i < n; i++) interaction_tick(w, dt, player);
}

int interaction_selftest(void) {
    int fails = 0;
#define CHECK(c, ...) do { if (!(c)) { printf("[RealEngine] FAIL: interaction: "), printf(__VA_ARGS__), printf("\n"), fails++; } } while (0)
    World w;
    if (!ecs_init(&w)) return 1;
    memset(&g_rec, 0, sizeof g_rec), memset(&g_call, 0, sizeof g_call), memset(g_keys, 0, sizeof g_keys);
    interaction_set_hooks(&(InteractHooks){ rec_event, rec_call, test_key });
    interaction_test_gamepad(0);
    // the looker: an actor (no Character, no camera): it looks along its rotation from 50 cm above its location
    Entity me = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_INTERACTOR));
    ecs_component_defaults(COMP_INTERACTOR, ecs_get(&w, me, COMP_INTERACTOR));
    ((CInteractor*)ecs_get(&w, me, COMP_INTERACTOR))->flags &= ~IAR_PROMPT; // (no widget for the test's world)
    CTransform* mt = ecs_get(&w, me, COMP_TRANSFORM);
    mt->location[2] = -50.0f; // (its eye at z 0)
    Entity ahead = cube(&w, 150, 0, 0, COMP_BIT(COMP_INTERACTABLE));       // straight ahead, 1.5 m
    Entity side = cube(&w, 100, 60, 0, COMP_BIT(COMP_INTERACTABLE));       // ahead and to the left (31 degrees off)
    Entity far = cube(&w, 600, 0, 0, COMP_BIT(COMP_INTERACTABLE));         // out of reach
    Entity behind = cube(&w, -120, 0, 0, COMP_BIT(COMP_INTERACTABLE));     // behind
    interaction_begin_play(&w);
    interaction_after_begin_play(&w, ENTITY_NONE);
    frames(&w, 1, 1.0f / 60.0f, me);
    CHECK(interaction_focused(&w, me) == ahead, "the one ahead focused (%u; ahead %u side %u far %u behind %u)", interaction_focused(&w, me), ahead, side, far, behind);
    CHECK(g_rec.n[IA_EV_FOCUS_BEGIN] == 1 && g_rec.last[IA_EV_FOCUS_BEGIN] == ahead, "On Focus Begin on it");
    // turning left 30 degrees: the side one (the ahead one is now 30 degrees off: the side one is centered)
    mt = ecs_get(&w, me, COMP_TRANSFORM);
    mt->rotation[1] = 31.0f;
    frames(&w, 1, 1.0f / 60.0f, me);
    CHECK(interaction_focused(&w, me) == side, "turned: the side one focused (%u)", interaction_focused(&w, me));
    CHECK(g_rec.n[IA_EV_FOCUS_END] == 1 && g_rec.last[IA_EV_FOCUS_END] == ahead && g_rec.n[IA_EV_FOCUS_BEGIN] == 2, "On Focus End on the old one, Begin on the new");
    // turning around: the one behind (1.2 m); far away is never focused
    mt->rotation[1] = 180.0f;
    frames(&w, 1, 1.0f / 60.0f, me);
    CHECK(interaction_focused(&w, me) == behind, "turned around: the one behind");
    mt->rotation[1] = 0.0f;
    // the priority wins over the centered one
    ((CInteractable*)ecs_get(&w, side, COMP_INTERACTABLE))->priority = 1;
    ((CInteractable*)ecs_get(&w, side, COMP_INTERACTABLE))->angle = 45.0f;
    frames(&w, 1, 1.0f / 60.0f, me);
    CHECK(interaction_focused(&w, me) == side, "Priority 1 wins (%u)", interaction_focused(&w, me));
    ((CInteractable*)ecs_get(&w, side, COMP_INTERACTABLE))->priority = 0;
    // disabled: not focused
    interaction_set_enabled(&w, side, false);
    frames(&w, 1, 1.0f / 60.0f, me);
    CHECK(interaction_focused(&w, me) == ahead, "disabled: the other focused");
    // the key: a press interacts once (held down: not again)
    memset(&g_rec, 0, sizeof g_rec);
    g_keys['E'] = true;
    frames(&w, 3, 1.0f / 60.0f, me);
    g_keys['E'] = false;
    frames(&w, 1, 1.0f / 60.0f, me);
    CHECK(g_rec.n[IA_EV_INTERACT] == 1 && g_rec.last[IA_EV_INTERACT] == ahead, "E pressed: On Interact once (%u)", g_rec.n[IA_EV_INTERACT]);
    // once only: used, then not focused
    CInteractable* oc = ecs_get(&w, ahead, COMP_INTERACTABLE);
    oc->flags = (uint16_t)((oc->flags | IA_ONCE) & ~IA_USED);
    CHECK(interaction_interact(&w, me, ahead) && !interaction_interact(&w, me, ahead), "once: the second time refused");
    frames(&w, 1, 1.0f / 60.0f, me);
    CHECK(interaction_focused(&w, me) != ahead, "used once: no longer focused");
    interaction_set_enabled(&w, ahead, true); // (enabled again: usable again)
    ((CInteractable*)ecs_get(&w, ahead, COMP_INTERACTABLE))->flags &= ~IA_ONCE;
    // the hold: 1 s held, the progress, nothing when let go early
    CInteractable* ac = ecs_get(&w, ahead, COMP_INTERACTABLE);
    ac->hold_time = 1.0f;
    frames(&w, 1, 0.05f, me);
    memset(&g_rec, 0, sizeof g_rec);
    g_keys['E'] = true;
    frames(&w, 10, 0.05f, me); // 0.5 s
    CHECK(g_rec.n[IA_EV_INTERACT] == 0 && fabsf(g_rec.progress - 0.5f) < 0.03f, "held 0.5 s: progress %.2f, not used yet", g_rec.progress);
    char text[64], key[16];
    float pr = 0.0f;
    bool can = false;
    CHECK(interaction_prompt(&w, me, text, sizeof text, key, sizeof key, &pr, &can) && can && !strcmp(key, "E") && fabsf(pr - 0.5f) < 0.03f,
          "the prompt: key \"%s\", hold %.2f", key, pr);
    g_keys['E'] = false;
    frames(&w, 1, 0.05f, me);
    CHECK(g_rec.progress == 0.0f && g_rec.n[IA_EV_INTERACT] == 0, "let go early: the progress back to 0, nothing used");
    g_keys['E'] = true;
    frames(&w, 21, 0.05f, me); // 1.05 s
    g_keys['E'] = false;
    frames(&w, 1, 0.05f, me);
    CHECK(g_rec.n[IA_EV_INTERACT] == 1, "held 1 s: used once (%u)", g_rec.n[IA_EV_INTERACT]);
    ac = ecs_get(&w, ahead, COMP_INTERACTABLE);
    ac->hold_time = 0.0f;
    // the condition: a Gameplay Tag on the instigator
    snprintf(ac->require, sizeof ac->require, "Key.Gold");
    ac->flags = (ac->flags & ~IA_REQ_INVENTORY) | IA_REQ_TAGS;
    CHECK(!interaction_interact(&w, me, ahead), "without the tag Key.Gold: refused");
    CHECK(interaction_prompt(&w, me, text, sizeof text, NULL, 0, NULL, &can) && !can && strstr(text, "Key.Gold"), "the prompt says what it needs: \"%s\"", text);
    gt_add(&w, me, "Key.Gold.Big");
    CHECK(interaction_interact(&w, me, ahead), "with Key.Gold.Big (under Key.Gold): used");
    gt_remove(&w, me, "Key.Gold.Big");
    CHECK(interaction_interact(&w, me, ahead), "unlocked once: stays unlocked");
    // the prompt's text: the verb, {0}, the gamepad's key
    ac = ecs_get(&w, ahead, COMP_INTERACTABLE);
    snprintf(ac->prompt, sizeof ac->prompt, "Talk to {0}"), snprintf(ac->name, sizeof ac->name, "Bob");
    interaction_test_gamepad(1);
    CHECK(interaction_prompt(&w, me, text, sizeof text, key, sizeof key, NULL, NULL) && !strcmp(text, "Talk to Bob") && !strcmp(key, "X"), "\"%s\" [%s] (Talk to Bob [X])", text, key);
    interaction_test_gamepad(0);
    // a door: swings 90 degrees about its hinge (the -X edge) in its time; away from who opens it
    Entity door = cube(&w, 0, 2000, 105, COMP_BIT(COMP_INTERACTABLE) | COMP_BIT(COMP_DEVICE));
    CTransform* dt0 = ecs_get(&w, door, COMP_TRANSFORM);
    dt0->scale[0] = 1.0f, dt0->scale[1] = 0.1f, dt0->scale[2] = 2.1f;
    CDevice* dd = ecs_get(&w, door, COMP_DEVICE);
    dd->amount = 90.0f, dd->time = 0.5f;
    Entity opener = cube(&w, 0, 2000 - 150, 50, 0); // (on the door's -Y side)
    CHECK(interaction_interact(&w, opener, door) && interaction_device_active(&w, door), "the door opens");
    frames(&w, 40, 0.025f, me); // 1 s
    const CTransform* dtr = ecs_get(&w, door, COMP_TRANSFORM);
    float hx = -50.0f; // (the hinge stays where it was: its corner at x -50, y 2000)
    CHECK(fabsf(fabsf(dtr->rotation[1]) - 90.0f) < 0.1f && fabsf(dtr->location[0] - hx) < 0.5f && fabsf(fabsf(dtr->location[1] - 2000.0f) - 50.0f) < 0.5f,
          "swung 90 degrees about the hinge (yaw %.1f, at %.1f %.1f)", dtr->rotation[1], dtr->location[0], dtr->location[1]);
    CHECK(dtr->location[1] > 2000.0f, "away from the opener (it stood at -Y: the door went to +Y, %.1f)", dtr->location[1]);
    CHECK(interaction_interact(&w, opener, door) && !interaction_device_active(&w, door), "closed again");
    frames(&w, 40, 0.025f, me);
    dtr = ecs_get(&w, door, COMP_TRANSFORM);
    CHECK(fabsf(dtr->rotation[1]) < 0.01f && fabsf(dtr->location[0]) < 0.01f && fabsf(dtr->location[1] - 2000.0f) < 0.01f, "back where it was");
    // a sliding door: 100 cm along X
    dd = ecs_get(&w, door, COMP_DEVICE);
    dd->motion = DEV_SLIDE, dd->amount = 100.0f;
    interaction_interact(&w, opener, door);
    frames(&w, 40, 0.025f, me);
    dtr = ecs_get(&w, door, COMP_TRANSFORM);
    CHECK(fabsf(dtr->location[0] - 100.0f) < 0.01f, "slid 100 cm (%.2f)", dtr->location[0]);
    // the auto close
    dd = ecs_get(&w, door, COMP_DEVICE);
    dd->auto_close = 0.5f;
    frames(&w, 30, 0.025f, me); // 0.75 s: closing
    frames(&w, 30, 0.025f, me);
    CHECK(!interaction_device_active(&w, door) && fabsf(((CTransform*)ecs_get(&w, door, COMP_TRANSFORM))->location[0]) < 0.01f, "closed by itself");
    // a lever switches its target: a device, a light, an actor; or a Custom Event with On
    Entity lever = cube(&w, 0, 3000, 30, COMP_BIT(COMP_INTERACTABLE) | COMP_BIT(COMP_DEVICE));
    interaction_setup_device(&w, lever, DEV_LEVER);
    Entity lamp = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT));
    Entity wall = cube(&w, 0, 3300, 50, 0);
    CDevice* ld = ecs_get(&w, lever, COMP_DEVICE);
    ld->target = door;
    ((CDevice*)ecs_get(&w, door, COMP_DEVICE))->auto_close = 0.0f;
    CHECK(interaction_interact(&w, opener, lever) && interaction_device_active(&w, lever) && interaction_device_active(&w, door), "the lever opens the door");
    CHECK(interaction_interact(&w, opener, lever) && !interaction_device_active(&w, lever) && !interaction_device_active(&w, door), "and closes it");
    ld = ecs_get(&w, lever, COMP_DEVICE);
    ld->target = lamp;
    interaction_interact(&w, opener, lever);
    const CLight* ll = ecs_get(&w, lamp, COMP_LIGHT);
    CHECK(ll->ext && !ll->affects_world, "the lever turns the light off");
    interaction_interact(&w, opener, lever);
    CHECK(ll->affects_world, "and on");
    ld = ecs_get(&w, lever, COMP_DEVICE);
    ld->target = wall;
    interaction_interact(&w, opener, lever);
    CHECK(((CTransform*)ecs_get(&w, wall, COMP_TRANSFORM))->flags & TRANSFORM_HIDDEN_GAME, "the lever hides an actor");
    ld = ecs_get(&w, lever, COMP_DEVICE);
    snprintf(ld->event, sizeof ld->event, "OpenGate");
    interaction_interact(&w, opener, lever); // (the 6th pull: off)
    CHECK(g_call.n == 1 && !strcmp(g_call.name, "OpenGate") && !g_call.on, "a Custom Event on the target with On false (%u, %s, %d)", g_call.n, g_call.name, (int)g_call.on);
    interaction_interact(&w, opener, lever);
    CHECK(g_call.n == 2 && g_call.on, "and On true the next pull (%u, %d)", g_call.n, (int)g_call.on);
    // the lever's handle tilts about its foot (which stays put)
    frames(&w, 30, 0.025f, me);
    const CTransform* lt = ecs_get(&w, lever, COMP_TRANSFORM);
    CHECK(fabsf(lt->rotation[0] - 35.0f) < 0.1f, "the handle tilted to +35 (%.1f)", lt->rotation[0]);
    // a button: in, then out by itself; it toggles its target each press
    Entity button = cube(&w, 0, 4000, 110, COMP_BIT(COMP_INTERACTABLE) | COMP_BIT(COMP_DEVICE));
    interaction_setup_device(&w, button, DEV_BUTTON);
    ((CDevice*)ecs_get(&w, button, COMP_DEVICE))->target = wall;
    CHECK(interaction_interact(&w, opener, button) && interaction_device_active(&w, button), "the button pressed");
    CHECK(!(((CTransform*)ecs_get(&w, wall, COMP_TRANSFORM))->flags & TRANSFORM_HIDDEN_GAME), "the button shows the actor again");
    frames(&w, 40, 0.025f, me);
    CHECK(!interaction_device_active(&w, button), "the button came back out");
    // the chest's lid rises at the front (the lid's back edge stays)
    Entity chest = cube(&w, 0, 5000, 25, COMP_BIT(COMP_INTERACTABLE) | COMP_BIT(COMP_DEVICE));
    snprintf(((CName*)ecs_get(&w, chest, COMP_NAME))->text, RE_NAME_MAX, "Chest");
    interaction_setup_device(&w, chest, DEV_CHEST);
    CDevice* cd = ecs_get(&w, chest, COMP_DEVICE);
    Entity lid = cd->part;
    CHECK(lid && !strcmp(((CName*)ecs_get(&w, lid, COMP_NAME))->text, "Chest_Lid"), "the chest's lid made");
    CHECK(interaction_interact(&w, opener, chest), "the chest opens (no inventory: nothing given)");
    frames(&w, 40, 0.025f, me);
    lt = ecs_get(&w, lid, COMP_TRANSFORM);
    float rows[3][4];
    render_transform_rows(lt, rows);
    float front_z = rows[2][0] * 50.0f + rows[2][3]; // (the lid's +X edge)
    CHECK(front_z > 100.0f, "the lid's front rose (z %.1f)", front_z);
    // 200 Interactables in a 20 x 20 m grid around the looker: the frame's focus, timed (notas-dev/interacao.md)
    ecs_shutdown(&w);
    if (!ecs_init(&w)) return fails + 1;
    me = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_INTERACTOR));
    ecs_component_defaults(COMP_INTERACTOR, ecs_get(&w, me, COMP_INTERACTOR));
    ((CInteractor*)ecs_get(&w, me, COMP_INTERACTOR))->flags &= ~(IAR_SIGHT | IAR_PROMPT);
    for (int i = 0; i < 200; i++) cube(&w, -1000.0f + (float)(i % 14) * 150.0f, -1000.0f + (float)(i / 14) * 150.0f, 0, COMP_BIT(COMP_INTERACTABLE));
    interaction_begin_play(&w);
    enum { FRAMES = 2000 };
    double t0 = re_time_seconds();
    for (int f = 0; f < FRAMES; f++) {
        ((CTransform*)ecs_get(&w, me, COMP_TRANSFORM))->rotation[1] = (float)(f % 360);
        interaction_tick(&w, 1.0f / 60.0f, me);
    }
    double ms = (re_time_seconds() - t0) * 1000.0 / FRAMES;
    printf("[RealEngine] interaction: 200 Interactables, the Interactor turning: %.4f ms a frame (%u in reach last frame, %u focus changes)\n", ms,
           G.st.candidates, G.st.focus_changes);
    CHECK(G.st.interactables == 200, "the walk saw the 200 Interactables (%u)", G.st.interactables);
    CHECK(ms < 0.1, "200 Interactables in %.4f ms a frame (the budget: 0.1 ms)", ms);
    interaction_end_play(&w);
    ecs_shutdown(&w);
    interaction_test_gamepad(-1);
#undef CHECK
    printf("[RealEngine] interaction: self-test %s\n", fails ? "FAILED" : "ok");
    return fails;
}
