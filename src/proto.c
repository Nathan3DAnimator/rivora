// Level prototyping (proto.h): each shape's boxes from its numbers, placed under the actor
#include "proto.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "components.h"
#include "material.h"
#include "render.h"

const char* const g_proto_kind_names[PROTO_KIND_COUNT] = { "Box", "Stairs", "Ramp", "Wall", "Room", "Corridor", "Platform" };

static char g_root[600];
static uint32_t g_grid; // M_Grid's hash (0: not found: the color)

void proto_init(const char* content_root) {
    snprintf(g_root, sizeof g_root, "%s", content_root ? content_root : "");
    g_grid = 0; // (found at the first build: the materials may not be ready yet)
}

static uint32_t grid_material(void) {
    if (!g_grid && g_root[0]) {
        char p[700];
        snprintf(p, sizeof p, "%s/Materials/M_Grid.rmat", g_root);
        Material* m = mat_load(p);
        g_grid = m ? m->hash : 0u;
    }
    return g_grid;
}

typedef struct PBox { float c[3], s[3], rot[2]; } PBox; // a box's middle and size, cm (the actor's space); rot: its own pitch, yaw (a spline's)
#define MAX_BOXES 256

static uint32_t add(PBox* b, uint32_t n, float x0, float y0, float z0, float x1, float y1, float z1) {
    if (n >= MAX_BOXES || x1 - x0 < 0.5f || y1 - y0 < 0.5f || z1 - z0 < 0.5f) return n;
    b[n] = (PBox){ { (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f }, { x1 - x0, y1 - y0, z1 - z0 }, { 0.0f, 0.0f } };
    return n + 1;
}

// A wall along x from x0 to x1 (thickness in y around y), height h, with its opening (width ow, height oh, its middle at
// ox from the wall's middle; ow 0: none)
static uint32_t wall(PBox* b, uint32_t n, float x0, float x1, float y, float t, float h, float ow, float oh, float ox) {
    float mid = (x0 + x1) * 0.5f;
    if (ow < 1.0f) return add(b, n, x0, y - t * 0.5f, 0.0f, x1, y + t * 0.5f, h);
    float a = fmaxf(mid + ox - ow * 0.5f, x0), c = fminf(mid + ox + ow * 0.5f, x1);
    n = add(b, n, x0, y - t * 0.5f, 0.0f, a, y + t * 0.5f, h);
    n = add(b, n, c, y - t * 0.5f, 0.0f, x1, y + t * 0.5f, h);
    return add(b, n, a, y - t * 0.5f, fminf(oh, h), c, y + t * 0.5f, h); // (the lintel)
}

// A wall along x with nd doors spread evenly (each door's middle in its share of the length, shifted by ox)
static uint32_t wall_doors(PBox* b, uint32_t n, float x0, float x1, float y, float t, float h, uint32_t nd, float ow, float oh, float ox) {
    if (nd <= 1 || ow < 1.0f) return wall(b, n, x0, x1, y, t, h, ow, oh, ox);
    float seg = (x1 - x0) / (float)nd;
    for (uint32_t i = 0; i < nd; i++) n = wall(b, n, x0 + seg * (float)i, x0 + seg * (float)(i + 1), y, t, h, fminf(ow, seg - 2.0f * t), oh, ox);
    return n;
}

// The same along y (x0..x1 now its span in y, at x): a room's side wall with its nd doors
static uint32_t wall_doors_y(PBox* b, uint32_t n, float y0, float y1, float x, float t, float h, uint32_t nd, float ow, float oh, float oy) {
    uint32_t n0 = n;
    n = wall_doors(b, n, y0, y1, x, t, h, nd, ow, oh, oy);
    for (uint32_t i = n0; i < n; i++) { // (built along x: x and y swapped)
        float c = b[i].c[0], s = b[i].s[0];
        b[i].c[0] = b[i].c[1], b[i].s[0] = b[i].s[1];
        b[i].c[1] = c, b[i].s[1] = s;
    }
    return n;
}

// --- Along a spline (2026-11-10) ---

// A box in a segment's frame: its middle at along / side / up from the segment's start (the frame's axes: fwd along it,
// side its horizontal left, up its up), its size along x (along), y (side), z (up)
static uint32_t add_framed(PBox* b, uint32_t n, const float o[3], const float ax[3][3], const float rot[2], float along, float side, float up, float sx, float sy, float sz) {
    if (n >= MAX_BOXES || sx < 0.5f || sy < 0.5f || sz < 0.5f) return n;
    for (int k = 0; k < 3; k++) b[n].c[k] = o[k] + ax[0][k] * along + ax[1][k] * side + ax[2][k] * up;
    b[n].s[0] = sx, b[n].s[1] = sy, b[n].s[2] = sz;
    b[n].rot[0] = rot[0], b[n].rot[1] = rot[1];
    return n + 1;
}

// The spline's points sampled (Catmull-Rom through them, a sample at most every ~120 cm, at least 1 per span)
static uint32_t spline_samples(const CProto* p, float (*out)[3], uint32_t cap) {
    uint32_t n = 0, np = p->npoints < PROTO_MAX_POINTS ? p->npoints : PROTO_MAX_POINTS;
    for (uint32_t i = 0; i + 1 < np; i++) {
        const float* p0 = p->points[i ? i - 1 : i];
        const float* p1 = p->points[i];
        const float* p2 = p->points[i + 1];
        const float* p3 = p->points[i + 2 < np ? i + 2 : i + 1];
        float d[3] = { p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2] };
        float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        uint32_t k = (uint32_t)ceilf(len / 120.0f);
        k = k < 1 ? 1 : k > 24 ? 24 : k;
        for (uint32_t j = 0; j < k && n < cap; j++) {
            float u = (float)j / (float)k, u2 = u * u, u3 = u2 * u;
            for (int c = 0; c < 3; c++)
                out[n][c] = 0.5f * ((2.0f * p1[c]) + (-p0[c] + p2[c]) * u + (2.0f * p0[c] - 5.0f * p1[c] + 4.0f * p2[c] - p3[c]) * u2 + (-p0[c] + 3.0f * p1[c] - 3.0f * p2[c] + p3[c]) * u3);
            n++;
        }
    }
    if (np && n < cap) memcpy(out[n++], p->points[np - 1], 12);
    return n;
}

static uint32_t build_spline(const CProto* p, PBox* b) {
    float W = fmaxf(p->size[1], 1.0f), H = fmaxf(p->size[2], 1.0f), t = fmaxf(p->thickness, 1.0f);
    static float sp[PROTO_MAX_POINTS * 25][3];
    uint32_t ns = spline_samples(p, sp, PROTO_MAX_POINTS * 25), n = 0;
    for (uint32_t i = 0; i + 1 < ns; i++) {
        float d[3] = { sp[i + 1][0] - sp[i][0], sp[i + 1][1] - sp[i][1], sp[i + 1][2] - sp[i][2] };
        float len = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1.0f) continue;
        float rot[2] = { asinf(fmaxf(fminf(d[2] / len, 1.0f), -1.0f)) * 57.29578f, atan2f(d[1], d[0]) * 57.29578f }; // (pitch, yaw)
        float ax[3][3];
        rotator_axes((float[3]){ rot[0], rot[1], 0.0f }, ax);
        float mid = len * 0.5f, over = len + t; // (a little longer: no gap where the curve turns)
        switch (p->kind) {
        case PROTO_WALL: n = add_framed(b, n, sp[i], ax, rot, mid, 0.0f, H * 0.5f, over, t, H); break;
        case PROTO_CORRIDOR: // its floor, its two walls (and its ceiling when asked)
            n = add_framed(b, n, sp[i], ax, rot, mid, 0.0f, 1.0f - t * 0.5f, over, W, t);
            n = add_framed(b, n, sp[i], ax, rot, mid, W * 0.5f - t * 0.5f, H * 0.5f, over, t, H);
            n = add_framed(b, n, sp[i], ax, rot, mid, -W * 0.5f + t * 0.5f, H * 0.5f, over, t, H);
            if (p->flags & PROTO_CEILING) n = add_framed(b, n, sp[i], ax, rot, mid, 0.0f, H + t * 0.5f, over, W, t);
            break;
        case PROTO_RAMP: n = add_framed(b, n, sp[i], ax, rot, mid, 0.0f, -t * 0.5f, over, W, t); break; // (a path: its top on the curve)
        case PROTO_PLATFORM: n = add_framed(b, n, sp[i], ax, rot, mid, 0.0f, H - t * 0.5f, over, W, t); break; // (a path in the air: legs below)
        default: n = add_framed(b, n, sp[i], ax, rot, mid, 0.0f, H * 0.5f, over, W, H); break; // (a Box: a solid strip)
        }
    }
    if (p->kind == PROTO_PLATFORM) { // a leg under each point the user placed
        float lt = fminf(t * 1.5f, W * 0.5f);
        for (uint32_t i = 0; i < p->npoints && i < PROTO_MAX_POINTS; i++)
            n = add(b, n, p->points[i][0] - lt * 0.5f, p->points[i][1] - lt * 0.5f, p->points[i][2], p->points[i][0] + lt * 0.5f, p->points[i][1] + lt * 0.5f, p->points[i][2] + H - t);
    }
    return n;
}

bool proto_spline_kind(uint32_t kind) {
    return kind == PROTO_WALL || kind == PROTO_CORRIDOR || kind == PROTO_BOX || kind == PROTO_RAMP || kind == PROTO_PLATFORM;
}

static uint32_t build(const CProto* p, PBox* b) {
    if (p->npoints >= 2 && proto_spline_kind(p->kind)) return build_spline(p, b);
    float L = fmaxf(p->size[0], 1.0f), W = fmaxf(p->size[1], 1.0f), H = fmaxf(p->size[2], 1.0f), t = fmaxf(p->thickness, 1.0f);
    uint32_t n = 0;
    switch (p->kind) {
    default:
    case PROTO_BOX: n = add(b, n, -L * 0.5f, -W * 0.5f, 0.0f, L * 0.5f, W * 0.5f, H); break;
    case PROTO_STAIRS:
    case PROTO_RAMP: { // solid steps up along x (a ramp: steps of at most 6 cm, a slope to look at, steps to walk)
        uint32_t s = p->kind == PROTO_RAMP ? (uint32_t)ceilf(H / 6.0f) : (p->steps ? p->steps : 10);
        if (s > MAX_BOXES) s = MAX_BOXES;
        for (uint32_t i = 0; i < s; i++)
            n = add(b, n, -L * 0.5f + L * (float)i / (float)s, -W * 0.5f, 0.0f, -L * 0.5f + L * (float)(i + 1) / (float)s, W * 0.5f, H * (float)(i + 1) / (float)s);
        break;
    }
    case PROTO_WALL: n = wall_doors(b, n, -L * 0.5f, L * 0.5f, 0.0f, t, H, PROTO_DOORS(p->flags), p->opening[0], p->opening[1], p->opening[2]); break;
    case PROTO_ROOM: // a floor, four walls inside its edges (the door in the front one, -y), a ceiling when asked
    case PROTO_CORRIDOR: { // a floor and the two long walls (along x), a ceiling when asked
        // (its top 1 cm above the actor: on a ground at the same height the two flickered, fighting for the depth)
        n = add(b, n, -L * 0.5f, -W * 0.5f, 1.0f - t, L * 0.5f, W * 0.5f, 1.0f);
        bool room = p->kind == PROTO_ROOM;
        // the front wall (-y): its doors (a corridor's too, when it has an opening: 2026-11-10); the back one (+y): a door when asked
        float front = room || (p->flags & PROTO_DOOR_FRONT) ? p->opening[0] : 0.0f;
        n = wall_doors(b, n, -L * 0.5f, L * 0.5f, -W * 0.5f + t * 0.5f, t, H, PROTO_DOORS(p->flags), front, p->opening[1], p->opening[2]);
        bool back = (p->flags & PROTO_DOOR_BACK) != 0;
        n = wall_doors(b, n, -L * 0.5f, L * 0.5f, W * 0.5f - t * 0.5f, t, H, PROTO_DOORS_AT(p->flags, PROTO_DOORS_BACK_SHIFT), back ? p->opening[0] : 0.0f,
                       p->opening[1], back ? p->opening[2] : 0.0f);
        if (room) { // the side walls between the long ones (their doors in the middle when asked)
            float ow = p->opening[0], oh = p->opening[1];
            n = wall_doors_y(b, n, -W * 0.5f + t, W * 0.5f - t, -L * 0.5f + t * 0.5f, t, H, PROTO_DOORS_AT(p->flags, PROTO_DOORS_LEFT_SHIFT),
                             (p->flags & PROTO_DOOR_LEFT) ? ow : 0.0f, oh, 0.0f);
            n = wall_doors_y(b, n, -W * 0.5f + t, W * 0.5f - t, L * 0.5f - t * 0.5f, t, H, PROTO_DOORS_AT(p->flags, PROTO_DOORS_RIGHT_SHIFT),
                             (p->flags & PROTO_DOOR_RIGHT) ? ow : 0.0f, oh, 0.0f);
        }
        if (p->flags & PROTO_CEILING) n = add(b, n, -L * 0.5f, -W * 0.5f, H, L * 0.5f, W * 0.5f, H + t);
        break;
    }
    case PROTO_PLATFORM: { // a slab at the height, a leg under each corner
        n = add(b, n, -L * 0.5f, -W * 0.5f, H - t, L * 0.5f, W * 0.5f, H);
        float lt = fminf(t * 1.5f, fminf(L, W) * 0.25f);
        for (int k = 0; k < 4; k++) {
            float x = k & 1 ? L * 0.5f - lt : -L * 0.5f, y = k & 2 ? W * 0.5f - lt : -W * 0.5f;
            n = add(b, n, x, y, 0.0f, x + lt, y + lt, H - t);
        }
        break;
    }
    }
    return n;
}

static uint32_t fnv(const void* p, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}

// The children of an owner (or every orphan: owner 0) go
static void destroy_children(World* w, Entity owner) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
        for (uint32_t r = 0; r < ar->count;) {
            const CAttach* at = &((const CAttach*)ecs_arch_column(ar, COMP_ATTACH))[r];
            if (at->kind == ATTACH_PROTO && (owner ? at->owner == owner : !ecs_alive(w, at->owner))) ecs_destroy(w, ecs_arch_entities(ar)[r]);
            else r++;
        }
    }
}

void proto_update(World* w) {
    static uint32_t s_changes = ~0u;
    if (w->changes == s_changes) return; // (nothing moved or changed: nothing to build)
    bool any = false;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_PROTO)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        any = true;
        for (uint32_t r = 0; r < ar->count; r++) {
            Entity e = ecs_arch_entities(ar)[r];
            CProto* p = ecs_get(w, e, COMP_PROTO);
            const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
            uint32_t h = fnv(p, offsetof(CProto, built), fnv(t, sizeof(float) * 6, fnv(&t->flags, 4, 2166136261u)));
            h = fnv(&p->npoints, sizeof p->npoints + sizeof(float) * 3 * (p->npoints < PROTO_MAX_POINTS ? p->npoints : PROTO_MAX_POINTS), h); // (the spline's points)
            if (h == p->built) continue;
            destroy_children(w, e);
            static PBox boxes[MAX_BOXES];
            uint32_t n = build(p, boxes);
            float ax[3][3];
            rotator_axes(t->rotation, ax);
            CTransform tt = *t; // (the creations may move the actor's row)
            CProto pp = *p;
            for (uint32_t i = 0; i < n; i++) {
                Entity c = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
                if (c == ENTITY_NONE) break;
                snprintf(((CName*)ecs_get(w, c, COMP_NAME))->text, RE_NAME_MAX, "%s_%u", g_proto_kind_names[pp.kind % PROTO_KIND_COUNT], i);
                CAttach* at = ecs_get(w, c, COMP_ATTACH);
                at->owner = e, at->parent = e, at->node = i, at->kind = ATTACH_PROTO;
                at->scale[0] = at->scale[1] = at->scale[2] = 1.0f;
                CTransform* ct = ecs_get(w, c, COMP_TRANSFORM);
                for (int k = 0; k < 3; k++)
                    ct->location[k] = tt.location[k] + ax[0][k] * boxes[i].c[0] + ax[1][k] * boxes[i].c[1] + ax[2][k] * boxes[i].c[2];
                memcpy(ct->rotation, tt.rotation, 12);
                ct->rotation[0] += boxes[i].rot[0], ct->rotation[1] += boxes[i].rot[1]; // (a spline's segment: its own direction)
                for (int k = 0; k < 3; k++) ct->scale[k] = boxes[i].s[k] / 100.0f; // (the cube: 1 m)
                ct->flags = tt.flags;
                CStaticMesh* sm = ecs_get(w, c, COMP_STATIC_MESH);
                sm->mesh = SHAPE_CUBE;
                memcpy(sm->base_color, pp.color, 12);
                sm->roughness = 0.7f, sm->cast_shadow = 1;
                sm->material = pp.material ? pp.material : grid_material();
                sm->uv_scale = fmaxf(fmaxf(boxes[i].s[0], boxes[i].s[1]), boxes[i].s[2]) / 100.0f; // (the grid: a square per meter)
            }
            p = ecs_get(w, e, COMP_PROTO);
            if (p) p->built = h;
        }
    }
    if (!any || w->changes != s_changes) destroy_children(w, 0); // (a deleted shape's boxes)
    s_changes = w->changes;
}
