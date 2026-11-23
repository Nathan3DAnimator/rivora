// (Q16) The Details' properties by name (props.h). The table is src/props_table.inc (tools/gen_props.py); the names
// are hashed once into a sorted array: a lookup is one normalization and a binary search (notas-dev/em-jogo.md)
#include "props.h"

#include <ctype.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "fpcam.h" // (FP_ON)

#define SZ_PK_F32 4
#define SZ_PK_FLAG 4
#define SZ_PK_BOOL 1
#define SZ_PK_U32 4
#define SZ_PK_I32 4
#define SZ_PK_U8 1
#define SZ_PK_U16 2
#define SZ_PK_STR 1

// the sizes the generator read from the headers, checked by the compiler (a field's type changed: run tools/gen_props.py)
#define P(comp, T, sec, label, field, kind, n, lo, hi) \
    _Static_assert(sizeof(((T*)0)->field) == (size_t)(n) * SZ_##kind, "src/props_table.inc: " #T "." #field " (run tools/gen_props.py)");
#define B(comp, T, sec, label, field, mask, inv, bytes) \
    _Static_assert(sizeof(((T*)0)->field) == (size_t)(bytes), "src/props_table.inc: " #T "." #field " (run tools/gen_props.py)");
#include "props_table.inc"
#undef P
#undef B

static const PropInfo g_props[] = {
#define P(comp, T, sec, label, field, kind, n, lo, hi) { comp, kind, n, (uint32_t)offsetof(T, field), sec, label, lo, hi, 0, 0, 0 },
#define B(comp, T, sec, label, field, mask, inv, bytes) { comp, PK_BIT, 1, (uint32_t)offsetof(T, field), sec, label, 0, 0, (uint32_t)(mask), inv, bytes },
#include "props_table.inc"
#undef P
#undef B
    // (by hand: the Details' Mobility buttons) 0 Static, 1 Stationary, 2 Movable
    { COMP_TRANSFORM, PK_BIT, 1, (uint32_t)offsetof(CTransform, flags), "Transform", "Mobility", 0, 2, TRANSFORM_MOBILITY_MASK, 2, 4 },
};
#define NPROP ((int)(sizeof g_props / sizeof g_props[0]))

int prop_count(void) { return NPROP; }
const PropInfo* prop_at(int i) { return i >= 0 && i < NPROP ? &g_props[i] : NULL; }

// a name as it is looked up: letters and digits, lower case; stop: '.' ends it (a qualifier), '(' too when cut
static uint64_t norm_hash(const char* s, const char** end, bool cut_paren) {
    uint64_t h = 1469598103934665603ull;
    for (; *s && *s != '.'; s++) {
        if (cut_paren && *s == '(') {
            while (*s && *s != '.') s++;
            break;
        }
        unsigned char c = (unsigned char)*s;
        if (!isalnum(c)) continue;
        h = (h ^ (uint64_t)tolower(c)) * 1099511628211ull;
    }
    if (end) *end = s;
    return h;
}

typedef struct Key { uint64_t h; uint16_t i; } Key;
static Key g_keys[NPROP * 2];
static int g_nkeys;
static uint64_t g_sec[NPROP], g_comp_name[NPROP];

static int key_cmp(const void* a, const void* b) {
    const Key *x = a, *y = b;
    return x->h < y->h ? -1 : x->h > y->h ? 1 : (int)x->i - (int)y->i;
}

static void build(void) {
    if (g_nkeys) return;
    for (int i = 0; i < NPROP; i++) {
        uint64_t full = norm_hash(g_props[i].label, NULL, false), cut = norm_hash(g_props[i].label, NULL, true);
        g_keys[g_nkeys++] = (Key){ full, (uint16_t)i };
        if (cut != full) g_keys[g_nkeys++] = (Key){ cut, (uint16_t)i }; // ("Max Force (0: any)" also as "Max Force")
        g_sec[i] = g_props[i].section[0] ? norm_hash(g_props[i].section, NULL, false) : 0;
        g_comp_name[i] = norm_hash(g_components[g_props[i].comp].name, NULL, false);
    }
    qsort(g_keys, (size_t)g_nkeys, sizeof *g_keys, key_cmp);
}

// the first entry with hash h (g_nkeys: none)
static int first_of(uint64_t h) {
    int lo = 0, hi = g_nkeys;
    while (lo < hi) {
        int m = (lo + hi) / 2;
        if (g_keys[m].h < h) lo = m + 1;
        else hi = m;
    }
    return lo < g_nkeys && g_keys[lo].h == h ? lo : g_nkeys;
}

// the candidates for a name: the label's hash, the qualifier's (0: none)
static void parse(const char* name, uint64_t* label, uint64_t* qual) {
    const char* dot = strchr(name, '.');
    if (dot) *qual = norm_hash(name, NULL, false), *label = norm_hash(dot + 1, NULL, false);
    else *qual = 0, *label = norm_hash(name, NULL, false);
}

const PropInfo* prop_find(int comp, const char* name) {
    if (!name) return NULL;
    build();
    uint64_t lh, qh;
    parse(name, &lh, &qh);
    for (int k = first_of(lh); k < g_nkeys && g_keys[k].h == lh; k++) {
        int i = g_keys[k].i;
        if (comp >= 0 && g_props[i].comp != comp) continue;
        if (qh && qh != g_sec[i] && qh != g_comp_name[i]) continue;
        return &g_props[i];
    }
    return NULL;
}

// the component's data on the actor, else on the first of its attached components (a Blueprint's parts)
static void* comp_on(World* w, Entity e, int comp, Entity* found) {
    void* p = ecs_get(w, e, (ComponentId)comp);
    if (p) {
        *found = e;
        return p;
    }
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_ATTACH)) || !(a->mask & COMP_BIT(comp))) continue;
        const CAttach* at = ecs_arch_column(a, COMP_ATTACH);
        for (uint32_t i = 0; i < a->count; i++)
            if (at[i].owner == e) {
                *found = ecs_arch_entities(a)[i];
                return (uint8_t*)ecs_arch_column(a, (ComponentId)comp) + (size_t)i * g_components[comp].size;
            }
    }
    return NULL;
}

// the property of that name the actor has (its own components first) and where it is
static const PropInfo* resolve(World* w, Entity e, const char* name, uint8_t** at, Entity* who) {
    if (!w || !e || !name || !ecs_alive(w, e)) return NULL;
    build();
    uint64_t lh, qh;
    parse(name, &lh, &qh);
    for (int pass = 0; pass < 2; pass++) // the actor's own components, then its attached ones'
        for (int k = first_of(lh); k < g_nkeys && g_keys[k].h == lh; k++) {
            const PropInfo* p = &g_props[g_keys[k].i];
            if (qh && qh != g_sec[g_keys[k].i] && qh != g_comp_name[g_keys[k].i]) continue;
            uint8_t* d = pass == 0 ? ecs_get(w, e, (ComponentId)p->comp) : comp_on(w, e, p->comp, who);
            if (!d) continue;
            if (pass == 0) *who = e;
            *at = d + p->offset;
            return p;
        }
    return NULL;
}

const PropInfo* prop_on(World* w, Entity e, const char* name) {
    uint8_t* d;
    Entity who = ENTITY_NONE;
    return resolve(w, e, name, &d, &who);
}

bool prop_set(World* w, Entity e, const char* name, float value, const float vec[3]) {
    uint8_t* d;
    Entity who = ENTITY_NONE;
    const PropInfo* p = resolve(w, e, name, &d, &who);
    if (!p) return false;
    float x = value, v[4] = { value, 0, 0, 0 };
    if (p->count > 1 && vec) memcpy(v, vec, sizeof(float) * 3);
    // (no clamp to the Details' range: a value typed there passes it too, Blender's way)
    switch (p->kind) {
    case PK_F32:
        if (p->count == 1) memcpy(d, &x, 4);
        else memcpy(d, v, (size_t)(p->count < 4 ? p->count : 4) * 4);
        break;
    case PK_FLAG: { uint32_t u = x != 0.0f; memcpy(d, &u, 4); break; }
    case PK_BOOL: *(bool*)d = x != 0.0f; break;
    case PK_U32: { uint32_t u = x > 0.0f ? (uint32_t)lroundf(x) : 0u; memcpy(d, &u, 4); break; }
    case PK_I32: { int32_t i = (int32_t)lroundf(x); memcpy(d, &i, 4); break; }
    case PK_U8: *d = (uint8_t)(x < 0.0f ? 0 : x > 255.0f ? 255 : lroundf(x)); break;
    case PK_U16: { uint16_t u = (uint16_t)(x < 0.0f ? 0 : x > 65535.0f ? 65535 : lroundf(x)); memcpy(d, &u, 2); break; }
    case PK_BIT: { // (the field's other bits kept)
        uint32_t f = 0;
        memcpy(&f, d, p->bytes);
        uint32_t want = p->inv == 2 ? ((uint32_t)lroundf(x < 0.0f ? 0.0f : x) & p->mask) : ((x != 0.0f) != (p->inv == 1)) ? p->mask : 0u;
        f = (f & ~p->mask) | want;
        memcpy(d, &f, p->bytes);
        break;
    }
    default: return false; // (text: not by a number)
    }
    ecs_touch(w, who);
    return true;
}

bool prop_get(World* w, Entity e, const char* name, float v[4]) {
    uint8_t* d;
    Entity who = ENTITY_NONE;
    const PropInfo* p = resolve(w, e, name, &d, &who);
    memset(v, 0, sizeof(float) * 4);
    if (!p) return false;
    switch (p->kind) {
    case PK_F32: memcpy(v, d, (size_t)(p->count < 4 ? p->count : 4) * 4); break;
    case PK_FLAG: case PK_U32: { uint32_t u; memcpy(&u, d, 4); v[0] = p->kind == PK_FLAG ? (u != 0) : (float)u; break; }
    case PK_BOOL: v[0] = *(bool*)d ? 1.0f : 0.0f; break;
    case PK_I32: { int32_t i; memcpy(&i, d, 4); v[0] = (float)i; break; }
    case PK_U8: v[0] = (float)*d; break;
    case PK_U16: { uint16_t u; memcpy(&u, d, 2); v[0] = (float)u; break; }
    case PK_BIT: {
        uint32_t f = 0;
        memcpy(&f, d, p->bytes);
        v[0] = p->inv == 2 ? (float)(f & p->mask) : (float)(((f & p->mask) != 0) != (p->inv == 1));
        break;
    }
    default: return false;
    }
    return true;
}
