// (Q16, notas-dev/em-jogo.md) The Details' properties by name in the game: every Details control that edits a
// component's field (src/props_table.inc, made by tools/gen_props.py from the Details' code) is read and written by
// its label (Set / Get Property by Name in the Blueprints): "Intensity", "Light.Intensity", "Character Movement.Max
// Walk Speed" (spaces and case don't matter). Changing a field marks the actor touched (the renderer re-reads it).
#pragma once
#include <stdbool.h>
#include <stdint.h>

#include "ecs.h"

// PK_BIT: a checkbox that is a bit of a field (mask; inv 1: the bit set means off; inv 2: the masked value itself, Mobility)
typedef enum PropKind { PK_F32 = 0, PK_FLAG, PK_BOOL, PK_U32, PK_I32, PK_U8, PK_U16, PK_STR, PK_BIT } PropKind;
typedef struct PropInfo {
    uint16_t comp;         // ComponentId
    uint8_t kind, count;   // PropKind; the values (3: a vector or a color)
    uint32_t offset;       // in the component
    const char* section;   // the Details' section ("": the component's name)
    const char* label;     // the Details' label (English)
    float lo, hi;          // the Details' range (lo == hi: none)
    uint32_t mask;         // PK_BIT: the field's bits
    uint8_t inv, bytes;    // PK_BIT: inverted (1) or the value (2); the field's bytes (2 or 4)
} PropInfo;

int prop_count(void);
const PropInfo* prop_at(int i);
// The property by its name; comp >= 0: only that component's. NULL: none
const PropInfo* prop_find(int comp, const char* name);
// On the actor (else the first of its components that has it: a Blueprint's light, its mesh): a number, a checkbox
// (value != 0) from value; a vector or a color from vec. False: no such property on it
bool prop_set(World* w, Entity e, const char* name, float value, const float vec[3]);
const PropInfo* prop_on(World* w, Entity e, const char* name); // the property of that name the actor reaches (NULL: none)
bool prop_get(World* w, Entity e, const char* name, float v[4]);
