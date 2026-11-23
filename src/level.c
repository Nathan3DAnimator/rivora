#include "level.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "platform.h"

#define DEG2RAD 0.017453292519943295f
#define LEVEL_MAGIC 0x4C564C52u /* "RLVL" */
#define LEVEL_VERSION 2u // 2: lights and emissive in physical units (1 still opens, converted)
typedef struct LevelHeader {
    uint32_t magic, version;
    LevelCamera camera;
    uint32_t blob_size, reserved;
} LevelHeader;

// Version 1 had arbitrary light units, seen at exposure 1: the same picture in physical units at EV100 ~14 is the
// old values times 1.2 * 2^14 ~ 20 000 (a sun's lux above the atmosphere: / ~0.85, its transmittance at noon)
static void units_from_v1(World* w) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (a->mask & COMP_BIT(COMP_LIGHT)) {
            CLight* l = ecs_arch_column(a, COMP_LIGHT);
            for (uint32_t i = 0; i < a->count; i++)
                if (l[i].type != LIGHT_SKY) l[i].intensity *= l[i].type == LIGHT_DIRECTIONAL ? 24000.0f : 20000.0f;
        }
        if (a->mask & COMP_BIT(COMP_STATIC_MESH)) {
            CStaticMesh* m = ecs_arch_column(a, COMP_STATIC_MESH);
            for (uint32_t i = 0; i < a->count; i++)
                for (int k = 0; k < 3; k++) m[i].emissive[k] *= 20000.0f; // nits
        }
    }
}

bool level_load(World* w, const char* path, LevelCamera* cam) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* buf = size > (long)sizeof(LevelHeader) ? malloc((size_t)size) : NULL; // a load, not a frame
    bool ok = buf && fread(buf, 1, (size_t)size, f) == (size_t)size;
    fclose(f);
    ok = ok && level_load_memory(w, buf, (size_t)size, cam);
    free(buf);
    return ok;
}

bool level_load_memory(World* w, const uint8_t* buf, size_t size, LevelCamera* cam) {
    LevelHeader h;
    if (!buf || size < sizeof h) return false;
    memcpy(&h, buf, sizeof h);
    bool ok = h.magic == LEVEL_MAGIC && h.version >= 1 && h.version <= LEVEL_VERSION && sizeof h + h.blob_size <= size;
    ok = ok && ecs_deserialize(w, buf + sizeof h, h.blob_size);
    if (ok && h.version == 1) units_from_v1(w);
    if (ok && cam) *cam = h.camera;
    return ok;
}

bool level_save(World* w, const LevelCamera* cam, const char* path, size_t* bytes) {
    size_t n = ecs_serialized_size(w);
    uint8_t* buf = malloc(sizeof(LevelHeader) + n); // a save, not a frame
    if (!buf) return false;
    LevelHeader h = { LEVEL_MAGIC, LEVEL_VERSION, *cam, (uint32_t)n, 0 };
    memcpy(buf, &h, sizeof h);
    ecs_serialize(w, buf + sizeof h);
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE* f = re_fopen(tmp, "wb");
    bool ok = f && fwrite(buf, 1, sizeof h + n, f) == sizeof h + n;
    if (f) ok = (fclose(f) == 0) && ok;
    ok = ok && re_replace_file(tmp, path); // all or nothing: a crash mid-save keeps the old file
    free(buf);
    if (ok && bytes) *bytes = sizeof h + n;
    return ok;
}

void level_rlight_path(const char* level, char* out, size_t cap) {
    snprintf(out, cap, "%s", level);
    char* dot = strrchr(out, '.');
    if (dot && !strchr(dot, '\\') && !strchr(dot, '/')) *dot = 0;
    strncat(out, ".rlight", cap - strlen(out) - 1);
}

void level_camera_basis(const LevelCamera* c, float fwd[3], float right[3], float up[3]) {
    float sp = sinf(c->pitch * DEG2RAD), cp = cosf(c->pitch * DEG2RAD);
    float sy = sinf(c->yaw * DEG2RAD), cy = cosf(c->yaw * DEG2RAD);
    fwd[0] = cp * cy; fwd[1] = cp * sy; fwd[2] = sp;
    right[0] = -sy; right[1] = cy; right[2] = 0.0f;
    up[0] = -sp * cy; up[1] = -sp * sy; up[2] = cp;
}

void level_roll_basis(float roll_deg, const float fwd[3], float right[3], float up[3]) {
    (void)fwd;
    float s = sinf(roll_deg * DEG2RAD), c = cosf(roll_deg * DEG2RAD), r[3], u[3];
    for (int k = 0; k < 3; k++) r[k] = right[k] * c - up[k] * s, u[k] = up[k] * c + right[k] * s; // (rotator_axes' Y and Z)
    for (int k = 0; k < 3; k++) right[k] = r[k], up[k] = u[k];
}

bool level_player_camera(World* w, LevelCamera* out) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        const uint64_t need = COMP_BIT(COMP_CAMERA) | COMP_BIT(COMP_TRANSFORM);
        if ((a->mask & need) != need || (a->mask & COMP_BIT(COMP_ATTACH))) continue; // a pawn's camera: when possessed
        const CCamera* c = ecs_arch_column(a, COMP_CAMERA);
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        for (uint32_t i = 0; i < a->count; i++)
            if (c[i].auto_activate) {
                memcpy(out->pos, t[i].location, sizeof out->pos);
                out->pitch = t[i].rotation[0];
                out->yaw = t[i].rotation[1];
                out->fov_deg = c[i].fov_deg;
                return true;
            }
    }
    return false;
}

bool level_player_start(World* w, LevelCamera* out) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_PLAYER_START)) || !(a->mask & COMP_BIT(COMP_TRANSFORM)) || !a->count) continue;
        const CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        memcpy(out->pos, t[0].location, sizeof out->pos);
        out->pos[2] += 70.0f;
        out->yaw = t[0].rotation[1];
        out->pitch = 0.0f;
        return true;
    }
    return false;
}
