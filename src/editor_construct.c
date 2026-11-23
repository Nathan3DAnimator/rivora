// The Construction Script in the editor (sugestão #64, notas-dev/blueprints.md): like Unreal's, a Blueprint actor's
// ConstructionScript runs in the editor's viewport when the actor is placed, moved or changed and when its class is
// compiled, so what it builds shows while the level is made. What it does is only shown, never authored: each actor
// with a Construction Script keeps what was authored (its components' data) apart from what the script made of it.
//   * Every frame each such actor's data is compared with what the script left (a few hundred bytes, memcmp): a
//     difference is the user's edit. The root's location, rotation and scale take the edit as a change of the authored
//     ones (the script may have set them); any other component that differs is taken as authored as it is now.
//   * Then the authored data is put back and the script runs again on it (bp_construct_in_editor: its own actor only,
//     no game). A script slower than 2 ms waits for the gizmo to be let go.
//   * Before the level is saved, Play starts or the undo history keeps the world, the authored data is put back
//     (editor_construct_restore): files, Play and Undo see the authored level; the next frame shows the script again.
// The components' world transforms are derived from their attachments every frame (bp_components_update): they are not
// compared, their CAttach (the authored relative transform) is.
#include "editor.h"

#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "debug_viewer.h"
#include "platform.h"

#define CS_MAX_ACTORS 2048
#define CS_MAX_ENTS 64 // an actor and its components
#define CS_SLOW_MS 2.0 // a script slower than this waits for the gizmo to be let go

// what a component of the actor or the actor itself keeps: everything but its name, its Blueprint link, the derived
// transform of a component and the state that moves by itself (a spring arm's lag, a skeletal pose, physics, AI)
static uint64_t kept_mask(uint64_t mask, bool root) {
    uint64_t skip = COMP_BIT(COMP_NAME) | COMP_BIT(COMP_BLUEPRINT) | COMP_BIT(COMP_SPRING_ARM) | COMP_BIT(COMP_SKELETAL_MESH) |
                    COMP_BIT(COMP_RIGID_BODY) | COMP_BIT(COMP_AI_AGENT);
    if (!root) skip |= COMP_BIT(COMP_TRANSFORM);
    return mask & ~skip;
}
static uint32_t blob_size(uint64_t m) {
    uint32_t n = 0;
    for (int c = 0; c < COMP_COUNT; c++)
        if (m >> c & 1u) n += g_components[c].size;
    return n;
}
static void blob_read(World* w, Entity e, uint64_t m, uint8_t* dst) {
    for (int c = 0; c < COMP_COUNT; c++)
        if (m >> c & 1u) {
            const void* p = ecs_get(w, e, (ComponentId)c);
            if (p) memcpy(dst, p, g_components[c].size);
            dst += g_components[c].size;
        }
}
static void blob_write(World* w, Entity e, uint64_t m, const uint8_t* src) {
    for (int c = 0; c < COMP_COUNT; c++)
        if (m >> c & 1u) {
            void* p = ecs_get(w, e, (ComponentId)c);
            if (p) memcpy(p, src, g_components[c].size);
            src += g_components[c].size;
        }
    ecs_touch(w, e);
}

typedef struct CsEnt {
    Entity e;
    uint64_t mask;  // the kept components
    uint32_t off, size; // its blobs in the actor's block: authored at off, shown at off + size
} CsEnt;
typedef struct CsActor {
    Entity actor;
    uint32_t cls;      // its class's hash (another class: tracked anew)
    uint32_t compiles; // g_bp_compiles when it ran
    uint32_t n;
    CsEnt ents[CS_MAX_ENTS];
    uint8_t* blobs;    // authored and shown, each entity's
    uint32_t bytes;
    double ms;         // its last run
} CsActor;

static struct {
    World* w;
    CsActor* a;
    uint32_t na, cap;
    uint8_t scratch[64 * 1024]; // the current data of one entity (compared, never kept)
    double ms_frame;            // this frame's runs (the Debug Viewer's)
} C;

static void forget_actor(CsActor* a) { free(a->blobs), memset(a, 0, sizeof *a); }

void editor_construct_forget(void) {
    for (uint32_t i = 0; i < C.na; i++) forget_actor(&C.a[i]);
    C.na = 0;
}

// The actor and its components (the entities whose attachment's owner it is)
static uint32_t gather(World* w, Entity actor, Entity* out) {
    uint32_t n = 0;
    out[n++] = actor;
    for (uint32_t ai = 0; ai < w->archetype_count && n < CS_MAX_ENTS; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count && n < CS_MAX_ENTS; r++)
            if (at[r].owner == actor) out[n++] = ents[r];
    }
    return n;
}

// Tracks the actor anew: what it has now is what was authored
static bool track(World* w, CsActor* a, Entity actor, uint32_t cls) {
    Entity ents[CS_MAX_ENTS];
    uint32_t n = gather(w, actor, ents), bytes = 0;
    forget_actor(a);
    a->actor = actor, a->cls = cls, a->n = n;
    for (uint32_t i = 0; i < n; i++) {
        CsEnt* e = &a->ents[i];
        e->e = ents[i], e->mask = kept_mask(ecs_mask(w, ents[i]), i == 0), e->size = blob_size(e->mask);
        if (e->size > sizeof C.scratch) e->mask = 0, e->size = 0;
        e->off = bytes, bytes += e->size * 2;
    }
    a->blobs = malloc(bytes ? bytes : 1); // (when an actor is tracked, not every frame)
    if (!a->blobs) return false;
    a->bytes = bytes;
    for (uint32_t i = 0; i < n; i++) blob_read(w, a->ents[i].e, a->ents[i].mask, a->blobs + a->ents[i].off);
    return true;
}

// The user's edits since the script ran taken into the authored data, which is put back in the world
static void merge_and_restore(World* w, CsActor* a) {
    for (uint32_t i = 0; i < a->n; i++) {
        CsEnt* e = &a->ents[i];
        if (!e->size || !ecs_alive(w, e->e)) continue;
        uint8_t* authored = a->blobs + e->off;
        const uint8_t* shown = authored + e->size;
        blob_read(w, e->e, e->mask, C.scratch);
        if (memcmp(C.scratch, shown, e->size)) { // edited: component by component
            uint32_t off = 0;
            for (int c = 0; c < COMP_COUNT; c++) {
                if (!(e->mask >> c & 1u)) continue;
                uint32_t sz = g_components[c].size;
                if (memcmp(C.scratch + off, shown + off, sz)) {
                    if (c == COMP_TRANSFORM) { // the root's: the edit as a change of the authored placement
                        CTransform* at = (CTransform*)(authored + off);
                        const CTransform* now = (const CTransform*)(C.scratch + off);
                        const CTransform* sh = (const CTransform*)(shown + off);
                        for (int k = 0; k < 3; k++) {
                            at->location[k] += now->location[k] - sh->location[k];
                            at->rotation[k] += now->rotation[k] - sh->rotation[k];
                            at->scale[k] += now->scale[k] - sh->scale[k];
                        }
                        at->flags = now->flags;
                    } else memcpy(authored + off, C.scratch + off, sz);
                }
                off += sz;
            }
        }
        blob_write(w, e->e, e->mask, authored);
    }
}

static void run(World* w, CsActor* a) {
    double t0 = re_time_seconds();
    bp_construct_in_editor(w, a->actor);
    for (uint32_t i = 0; i < a->n; i++) // what the script left: the next frames compare against it
        if (a->ents[i].size && ecs_alive(w, a->ents[i].e)) blob_read(w, a->ents[i].e, a->ents[i].mask, a->blobs + a->ents[i].off + a->ents[i].size);
    a->compiles = g_bp_compiles;
    a->ms = (re_time_seconds() - t0) * 1000.0;
    C.ms_frame += a->ms;
}

static CsActor* find(Entity actor) {
    for (uint32_t i = 0; i < C.na; i++)
        if (C.a[i].actor == actor) return &C.a[i];
    return NULL;
}

void editor_construct_restore(Editor* ed) {
    World* w = &ed->world;
    if (C.w == w)
        for (uint32_t i = 0; i < C.na; i++)
            if (ecs_alive(w, C.a[i].actor)) merge_and_restore(w, &C.a[i]);
    editor_construct_forget(); // (the next frame tracks and runs them again)
}

double editor_construct_ms(void) { return C.ms_frame; }

// The Debug Viewer's list (F10): each actor's last run of its Construction Script
static void collect(DvList* l, const DvContext* c) {
    (void)c;
    l->ms = C.ms_frame;
    for (uint32_t i = 0; i < C.na; i++) {
        const CName* nm = C.w && ecs_alive(C.w, C.a[i].actor) ? ecs_get(C.w, C.a[i].actor, COMP_NAME) : NULL;
        dv_add(l, nm ? nm->text : "?", "", (float)C.a[i].ms, C.a[i].actor);
    }
}

void editor_construct_update(Editor* ed) {
    World* w = &ed->world;
    static bool registered;
    if (!registered) dv_register("Construction Scripts", collect), registered = true;
    C.ms_frame = 0.0;
    if (ed->playing) return;
    if (C.w != w) editor_construct_forget(), C.w = w;
    // the actors gone (deleted, another level) are forgotten
    for (uint32_t i = 0; i < C.na;) {
        const CBlueprint* b = ecs_alive(w, C.a[i].actor) ? ecs_get(w, C.a[i].actor, COMP_BLUEPRINT) : NULL;
        if (!b || b->asset_hash != C.a[i].cls) {
            forget_actor(&C.a[i]);
            C.a[i] = C.a[--C.na];
            memset(&C.a[C.na], 0, sizeof C.a[C.na]);
        } else i++;
    }
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_BLUEPRINT)) || (ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
        const CBlueprint* bl = ecs_arch_column(ar, COMP_BLUEPRINT);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count; r++) {
            BpAsset* cls = bp_find_hash(bl[r].asset_hash);
            if (!cls || !bp_has_construction(cls)) continue;
            Entity actor = ents[r];
            CsActor* a = find(actor);
            bool fresh = false;
            if (!a) { // a new one: tracked as it is (what was authored), then the script runs
                if (C.na == C.cap) {
                    uint32_t nc = C.cap ? C.cap * 2 : 64;
                    if (nc > CS_MAX_ACTORS) continue;
                    CsActor* q = realloc(C.a, nc * sizeof *q);
                    if (!q) continue;
                    memset(q + C.cap, 0, (nc - C.cap) * sizeof *q);
                    C.a = q, C.cap = nc;
                }
                a = &C.a[C.na++];
                if (!track(w, a, actor, bl[r].asset_hash)) {
                    C.na--;
                    continue;
                }
                fresh = true;
            }
            // what changed: the class's code, the components (rebuilt: tracked anew), the data (the user's edit)
            Entity now[CS_MAX_ENTS];
            uint32_t n = gather(w, actor, now);
            bool set_changed = n != a->n;
            for (uint32_t i = 0; i < n && !set_changed; i++)
                set_changed = now[i] != a->ents[i].e || kept_mask(ecs_mask(w, now[i]), i == 0) != a->ents[i].mask;
            bool edited = false;
            for (uint32_t i = 0; i < a->n && !edited && !set_changed && !fresh; i++) {
                const CsEnt* e = &a->ents[i];
                if (!e->size) continue;
                blob_read(w, e->e, e->mask, C.scratch);
                edited = memcmp(C.scratch, a->blobs + e->off + e->size, e->size) != 0;
            }
            bool recompiled = a->compiles != g_bp_compiles;
            if (!fresh && !set_changed && !edited && !recompiled) continue;
            if (!fresh && a->ms > CS_SLOW_MS && ed->gizmo_active) continue; // (a slow one: when the gizmo is let go)
            if (set_changed) { // the components were made again (the class changed): the root's edit kept, the rest anew
                merge_and_restore(w, a); // (the root: its entity is the same)
                track(w, a, actor, bl[r].asset_hash);
            } else if (!fresh) merge_and_restore(w, a);
            run(w, a);
        }
    }
}
