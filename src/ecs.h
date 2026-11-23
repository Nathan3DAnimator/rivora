// ECS by archetypes. An entity is a handle (slot index + generation); the entities with the same set of components
// share an archetype, which keeps one contiguous column per component (row i of every column = entity i of the
// archetype). Systems are plain functions that walk the columns of the archetypes matching a mask.
// Memory: every column and table is a reservation (mem.h) committed as it fills: no malloc, no moves, stable pointers.
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "components.h"
#include "mem.h"

#define RE_ECS_INDEX_BITS 20
#define RE_ECS_MAX_ENTITIES (1u << RE_ECS_INDEX_BITS) // 1 048 576
#define RE_ECS_MAX_ARCHETYPES 64
#define RE_ECS_CHANGE_RING 4096

// index in the low 20 bits, generation in the high 12 (a handle of a destroyed entity stops matching its slot)
typedef uint32_t Entity;
#define ENTITY_NONE 0u // slot 0 is never used, so a zeroed handle is "no entity"
static inline uint32_t entity_index(Entity e) { return e & (RE_ECS_MAX_ENTITIES - 1); }
static inline uint32_t entity_gen(Entity e) { return e >> RE_ECS_INDEX_BITS; }

typedef struct EcsSlot {
    uint32_t gen;       // bumped when the entity is destroyed
    uint16_t archetype; // ECS_NO_ARCHETYPE: free
    uint16_t pad;
    uint32_t row;
} EcsSlot;
#define ECS_NO_ARCHETYPE 0xFFFFu

typedef struct EcsArchetype {
    uint64_t mask;
    uint32_t count;
    ReVmBuf entities;              // Entity per row
    ReVmBuf columns[COMP_COUNT];   // reserved only for the components in mask
} EcsArchetype;

typedef struct World {
    ReVmBuf slots;      // EcsSlot per entity index
    uint32_t slot_count; // high-water mark (slot 0 reserved)
    ReVmBuf free_list;  // uint32_t indices of free slots
    uint32_t free_count;
    uint32_t alive;
    EcsArchetype archetypes[RE_ECS_MAX_ARCHETYPES];
    uint32_t archetype_count;
    // The change log the renderer follows (render.c keeps a copy of the scene on the GPU): every entity created,
    // destroyed, moved between archetypes or touched goes in a ring; a bulk change (a level loaded, a snapshot
    // restored, more than the ring holds) bumps the revision instead
    uint32_t revision;
    uint32_t changes; // entries written so far (the ring's head)
    uint32_t changed[RE_ECS_CHANGE_RING];
    // Relevance (the renderer's, a frame or two late): a bit per slot, set when the actor can show up in the view (seen,
    // its shadow seen, near enough to light it or be reflected). NULL: everything counts. Systems that cost (animation,
    // armatures) skip or slow down what isn't relevant (ecs_relevant)
    const uint32_t* relevant;
    uint32_t relevant_slots;
} World;

bool ecs_init(World* w);
void ecs_shutdown(World* w);

// An entity whose components were edited in place (the renderer re-reads it); ecs_touch_all: everything
void ecs_touch(World* w, Entity e);
void ecs_touch_all(World* w);

// A new entity with the components of mask, zeroed (a Transform starts with scale 1). ENTITY_NONE when full.
Entity ecs_create(World* w, uint64_t mask);
void ecs_destroy(World* w, Entity e);
bool ecs_alive(const World* w, Entity e);
uint64_t ecs_mask(const World* w, Entity e);
// The entity's component, or NULL when it doesn't have it (or is not alive).
void* ecs_get(World* w, Entity e, ComponentId c);
// A fresh component's defaults written to p (g_components[c].size bytes; the rest zero)
void ecs_component_defaults(ComponentId c, void* p);
// Moves the entity to the archetype of its components plus / minus one (the components kept are copied).
bool ecs_add(World* w, Entity e, ComponentId c);
bool ecs_remove(World* w, Entity e, ComponentId c);
// A copy of the entity with every component.
Entity ecs_clone(World* w, Entity e);

// The live slot of index i (0 < i < slot_count), for walks in a stable order (the Outliner); ENTITY_NONE when free.
Entity ecs_entity_at(const World* w, uint32_t index);

static inline EcsSlot* ecs_slots(const World* w) { return (EcsSlot*)w->slots.base; }
static inline Entity* ecs_arch_entities(const EcsArchetype* a) { return (Entity*)a->entities.base; }
static inline void* ecs_arch_column(const EcsArchetype* a, ComponentId c) { return a->columns[c].base; }

// Snapshot of the whole world by memcpy of its arrays (Play in Editor: taken when play starts, restored when it ends).
typedef struct WorldSnapshot {
    ReVmBuf slots, free_list;
    uint32_t slot_count, free_count, alive, archetype_count;
    uint32_t counts[RE_ECS_MAX_ARCHETYPES];
    ReVmBuf entities[RE_ECS_MAX_ARCHETYPES];
    ReVmBuf columns[RE_ECS_MAX_ARCHETYPES][COMP_COUNT];
    uint64_t bytes; // copied by the last save
} WorldSnapshot;

// The world as one compact, versioned blob (little endian): the undo history and the level files (.rlevel).
// Holds the component sizes it was written with; a blob from a different layout is refused (until a migration).
#define ECS_BLOB_MAGIC 0x444C5752u /* "RWLD" */
#define ECS_BLOB_VERSION 1u
size_t ecs_serialized_size(const World* w);
size_t ecs_serialize(const World* w, uint8_t* dst); // writes ecs_serialized_size bytes
// Replaces the world's contents with the blob's (same handles). False (world unchanged) when the blob is invalid.
bool ecs_deserialize(World* w, const uint8_t* src, size_t size);

bool ecs_snapshot_save(const World* w, WorldSnapshot* s);
// Back to the saved state: entities created since are gone, destroyed ones come back with the same handles.
void ecs_snapshot_restore(World* w, const WorldSnapshot* s);
void ecs_snapshot_free(WorldSnapshot* s);

// Whether an entity can show up in the view (World.relevant): true when unknown
static inline bool ecs_relevant(const World* w, Entity e) {
    uint32_t i = entity_index(e);
    return !w->relevant || i >= w->relevant_slots || (w->relevant[i >> 5] >> (i & 31u) & 1u) != 0u;
}
