// (Q11) The toon and the outline changed on an actor while the game runs (the Blueprint nodes Set Toon Parameter, Set
// Toon Color, Get Toon Parameter, Set Actor Outline, Set Light Toon Mode; toon_actor.h, notas-dev/toon.md)
#include "toon_actor.h"

#include <math.h>
#include <string.h>

#include "anim.h"
#include "components.h"
#include "material.h"

// The entities an actor draws with: itself (a Static Mesh) and its Skeletal Mesh's parts and skins
static uint32_t mesh_entities(World* w, Entity e, Entity* out, uint32_t cap) {
    if (!ecs_alive(w, e)) return 0;
    uint32_t n = 0;
    if (ecs_get(w, e, COMP_STATIC_MESH)) out[n++] = e;
    return n + anim_mesh_parts(w, e, out + n, cap - n);
}

static bool set_field(World* w, Entity e, const char* name, float v, const float* rgb) {
    Entity t[96];
    uint32_t n = mesh_entities(w, e, t, 96);
    bool ok = false;
    for (uint32_t i = 0; i < n; i++) {
        const CStaticMesh* sm = ecs_get(w, t[i], COMP_STATIC_MESH);
        Material* m = sm && sm->material ? mat_find_hash(sm->material) : NULL;
        if (!m) continue;
        uint32_t slot = mat_dynamic_for(t[i], m); // (its own instance: the other actors of the material keep theirs)
        const MatToon* cur = mat_toon_slot(slot);
        MatToon tt = cur ? *cur : *mat_toon_values(m);
        bool f = rgb ? toon_set_color(&tt, name, rgb) : toon_set_scalar(&tt, name, v);
        if (f) mat_toon_slot_set(slot, &tt), ok = true;
    }
    return ok;
}

bool toon_actor_set_scalar(World* w, Entity e, const char* name, float v) { return set_field(w, e, name, v, NULL); }
bool toon_actor_set_color(World* w, Entity e, const char* name, const float rgb[3]) { return set_field(w, e, name, 0.0f, rgb); }

bool toon_actor_get_scalar(World* w, Entity e, const char* name, float* v) {
    Entity t[96];
    uint32_t n = mesh_entities(w, e, t, 96);
    for (uint32_t i = 0; i < n; i++) {
        const CStaticMesh* sm = ecs_get(w, t[i], COMP_STATIC_MESH);
        Material* m = sm && sm->material ? mat_find_hash(sm->material) : NULL;
        if (!m) continue;
        const MatToon* cur = mat_toon_slot(mat_slot_of(t[i], m));
        if (!cur) cur = mat_toon_values(m);
        if (cur && toon_get_scalar(cur, name, v)) return true;
    }
    return false;
}

static uint32_t srgb_byte(float c) {
    c = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
    float s = c <= 0.0031308f ? c * 12.92f : 1.055f * powf(c, 1.0f / 2.4f) - 0.055f;
    return (uint32_t)(s * 255.0f + 0.5f);
}

uint32_t toon_outline_pack(int mode, const float rgb[3]) {
    return ((uint32_t)(mode & 0xFF) << 24) | srgb_byte(rgb[0]) << 16 | srgb_byte(rgb[1]) << 8 | srgb_byte(rgb[2]);
}
void toon_outline_color(uint32_t packed, float rgb[3]) {
    for (int k = 0; k < 3; k++) {
        float c = (float)((packed >> (16 - 8 * k)) & 255u) / 255.0f;
        rgb[k] = c <= 0.04045f ? c / 12.92f : powf((c + 0.055f) / 1.055f, 2.4f);
    }
}

bool toon_actor_set_outline(World* w, Entity e, int mode, const float rgb[3], float width) {
    Entity t[96];
    uint32_t n = mesh_entities(w, e, t, 96);
    for (uint32_t i = 0; i < n; i++) {
        CStaticMesh* sm = ecs_get(w, t[i], COMP_STATIC_MESH);
        if (!sm) continue;
        sm->outline = toon_outline_pack(mode, rgb);
        sm->outline_width = width > 0.0f ? width : 0.0f;
        ecs_touch(w, t[i]); // (the renderer marks its slot again)
    }
    return n > 0;
}

bool toon_light_set_mode(World* w, Entity e, int mode) {
    CLight* l = ecs_alive(w, e) ? ecs_get(w, e, COMP_LIGHT) : NULL;
    if (!l) return false;
    l->toon_mode = (uint32_t)(mode < 0 ? 0 : mode > 2 ? 2 : mode);
    ecs_touch(w, e);
    return true;
}
