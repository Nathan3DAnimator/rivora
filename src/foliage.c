// Foliage (foliage.h, notas-dev/landscape.md): the painted instances, their entities, the brush, Fill, the file
#include "foliage.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "landscape.h"
#include "platform.h"
#include "platform_files.h"

#define FOL_MAX 4                // Foliage actors at once (a level has one, like Unreal's)
#define FOL_INSTANCES 60000u     // instances per actor at most
#define RFOL_MAGIC 0x4C4F4652u   // "RFOL"

typedef struct FolInst {
    float pos[3], yaw, sxy, sz;
    uint32_t type;
} FolInst;

typedef struct Fol {
    Entity e;
    char name[32];
    FolInst* inst;
    Entity* ents;                // inst[i]'s entity
    uint32_t n, cap;
    uint32_t look[FOLIAGE_TYPES]; // each type's look when its entities were made (a change: they follow)
    bool alive, loaded, save;
    uint32_t rng;
} Fol;

static struct {
    char root[600];
    Fol fol[FOL_MAX];
    uint32_t nfol;
    FoliageStats stats;
    uint32_t revision;            // the world's when the entities were made (a bulk change - undo, a snapshot restored -: made again)
    uint32_t near[8192];         // the brush's instances inside it (this stroke's frame)
} F;

void foliage_init(const char* content_root) {
    for (uint32_t i = 0; i < F.nfol; i++) free(F.fol[i].inst), free(F.fol[i].ents);
    memset(&F, 0, sizeof F);
    snprintf(F.root, sizeof F.root, "%s", content_root ? content_root : "");
}

const FoliageStats* foliage_stats(void) { return &F.stats; }

static uint32_t fnv(const void* p, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static float rnd(uint32_t* s) {
    *s = *s * 747796405u + 2891336453u;
    uint32_t x = ((*s >> ((*s >> 28) + 4)) ^ *s) * 277803737u;
    return (float)(((x >> 22) ^ x) & 0xFFFFFF) / 16777216.0f;
}

static void path_of(const Fol* f, char* out, size_t cap) { snprintf(out, cap, "%s/Foliage/%s.rfol", F.root, f->name); }

static bool reserve(Fol* f, uint32_t n) {
    if (n <= f->cap) return true;
    if (n > FOL_INSTANCES) return false;
    uint32_t cap = f->cap ? f->cap : 1024;
    while (cap < n) cap *= 2;
    if (cap > FOL_INSTANCES) cap = FOL_INSTANCES;
    FolInst* a = realloc(f->inst, sizeof(FolInst) * cap); // (an edit's growth, not a frame)
    if (!a) return false;
    f->inst = a;
    Entity* b = realloc(f->ents, sizeof(Entity) * cap);
    if (!b) return false;
    f->ents = b, f->cap = cap;
    return true;
}

static bool load(Fol* f) {
    char p[800];
    path_of(f, p, sizeof p);
    FILE* fp = re_fopen(p, "rb");
    if (!fp) return false;
    uint32_t hd[3] = { 0 };
    bool ok = fread(hd, 4, 3, fp) == 3 && hd[0] == RFOL_MAGIC && hd[1] == 1u && hd[2] <= FOL_INSTANCES && reserve(f, hd[2]);
    ok = ok && fread(f->inst, sizeof(FolInst), hd[2], fp) == hd[2];
    fclose(fp);
    f->n = ok ? hd[2] : 0;
    return ok;
}

static void save(Fol* f) {
    char dir[700], p[800], tmp[820];
    snprintf(dir, sizeof dir, "%s/Foliage", F.root);
    re_make_dirs(dir);
    path_of(f, p, sizeof p);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE* fp = re_fopen(tmp, "wb");
    if (!fp) return;
    uint32_t hd[3] = { RFOL_MAGIC, 1u, f->n };
    bool ok = fwrite(hd, 4, 3, fp) == 3 && fwrite(f->inst, sizeof(FolInst), f->n, fp) == f->n;
    ok = fclose(fp) == 0 && ok;
    if (ok) re_replace_file(tmp, p); // (all or nothing)
    f->save = false;
}

// --- The entities ---

static void destroy_children(World* w, Entity owner) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH))) continue;
        for (uint32_t r = 0; r < ar->count; r++) {
            const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
            if (at[r].kind == ATTACH_FOLIAGE && (at[r].owner == owner || !ecs_alive(w, at[r].owner))) {
                ecs_destroy(w, ecs_arch_entities(ar)[r]);
                r--; // (the last row moved here)
            }
        }
    }
}

static uint32_t look_of(const FoliageType* t) {
    return fnv(&t->mesh, 8, fnv(t->color, 12, fnv(&t->wind, 4, 2166136261u)));
}

static void style(World* w, Entity e, const FoliageType* t) {
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    if (!sm) return;
    sm->mesh = t->mesh, sm->material = t->material, sm->uv_scale = 1.0f, sm->roughness = 0.8f;
    memcpy(sm->base_color, t->color, 12);
    sm->cast_shadow = 1u | CAST_SHADOW_NO_OCCLUDER | (t->wind > 0.0f ? CAST_SHADOW_WIND : 0u);
}

static Entity make_entity(World* w, Fol* f, const CFoliage* c, const FolInst* in) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "Foliage");
    CAttach* at = ecs_get(w, e, COMP_ATTACH);
    at->owner = at->parent = f->e, at->kind = ATTACH_FOLIAGE, at->node = in->type;
    at->scale[0] = at->scale[1] = at->scale[2] = 1.0f;
    style(w, e, &c->types[in->type < c->ntypes ? in->type : 0]);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memcpy(t->location, in->pos, 12);
    t->rotation[1] = in->yaw;
    t->scale[0] = t->scale[1] = in->sxy, t->scale[2] = in->sz;
    t->flags = MOBILITY_STATIC;
    return e;
}

static Fol* fol_of(Entity e) {
    for (uint32_t i = 0; i < F.nfol; i++)
        if (F.fol[i].e == e && F.fol[i].loaded) return &F.fol[i];
    return NULL;
}

// An instance added (its entity with it) / the i-th removed (the last moves into its place)
static bool add(World* w, Fol* f, const CFoliage* c, const FolInst* in) {
    if (!reserve(f, f->n + 1)) return false;
    f->inst[f->n] = *in;
    f->ents[f->n] = make_entity(w, f, c, in);
    f->n++, f->save = true;
    return true;
}
static void remove_at(World* w, Fol* f, uint32_t i) {
    ecs_destroy(w, f->ents[i]);
    f->n--;
    f->inst[i] = f->inst[f->n], f->ents[i] = f->ents[f->n];
    f->save = true;
}

// --- The ground under a point: the landscapes (their normal's up for the slope), else the brush's plane ---

static bool ground(float x, float y, float plane_z, bool plane, float* z, float* up) {
    if (!landscape_height(x, y, z)) {
        if (!plane) return false;
        *z = plane_z, *up = 1.0f;
        return true;
    }
    float za, zb, zc, zd, s = 50.0f;
    if (!landscape_height(x - s, y, &za)) za = *z;
    if (!landscape_height(x + s, y, &zb)) zb = *z;
    if (!landscape_height(x, y - s, &zc)) zc = *z;
    if (!landscape_height(x, y + s, &zd)) zd = *z;
    float nx = za - zb, ny = zc - zd, nz = 2.0f * s;
    *up = nz / sqrtf(nx * nx + ny * ny + nz * nz);
    return true;
}

static void place(FolInst* in, const FoliageType* t, uint32_t type, float x, float y, float z, uint32_t* rng) {
    float smin = t->scale[0] > 0.01f ? t->scale[0] : 0.3f, smax = fmaxf(t->scale[1], smin), s = smin + (smax - smin) * rnd(rng);
    in->pos[0] = x, in->pos[1] = y, in->pos[2] = z - 2.0f; // (a little into the ground: no gap on a slope)
    in->yaw = rnd(rng) * 360.0f;
    in->sxy = s, in->sz = s * (t->stretch > 0.05f ? t->stretch : 1.0f) * (0.85f + 0.3f * rnd(rng));
    in->type = type;
}

static float spacing_of(const FoliageType* t, float density_scale) { // cm: no two of a type nearer (a third of the mean)
    float d = fmaxf(t->density * density_scale, 0.01f);
    return sqrtf(100.0f / d) * 100.0f * 0.35f;
}

void foliage_paint(World* w, Entity e, const float at[3], float radius, float density_scale, bool erase, float dt) {
    Fol* f = fol_of(e);
    CFoliage* c = ecs_get(w, e, COMP_FOLIAGE);
    if (!f || !c || radius < 1.0f) return;
    double t0 = re_time_seconds();
    float r2 = radius * radius;
    // the instances inside the brush (of the painted types)
    uint32_t nn = 0;
    for (uint32_t i = 0; i < f->n; i++) {
        float dx = f->inst[i].pos[0] - at[0], dy = f->inst[i].pos[1] - at[1];
        uint32_t ty = f->inst[i].type;
        if (dx * dx + dy * dy < r2 && ty < c->ntypes && c->types[ty].paint) {
            if (erase) remove_at(w, f, i--);
            else if (nn < 8192) F.near[nn++] = i;
        }
    }
    if (erase) {
        F.revision = w->revision;
        F.stats.edit_ms = (re_time_seconds() - t0) * 1000.0;
        return;
    }
    float area_m2 = 3.14159265f * r2 / 10000.0f;
    for (uint32_t ty = 0; ty < c->ntypes; ty++) {
        const FoliageType* t = &c->types[ty];
        if (!t->paint || t->density <= 0.0f) continue;
        uint32_t have = 0;
        for (uint32_t k = 0; k < nn; k++) have += f->inst[F.near[k]].type == ty;
        float want = area_m2 / 100.0f * t->density * density_scale;
        if ((float)have >= want) continue;
        // the brush fills up over a fifth of a second held (Unreal's paint: up to the density, not past it)
        uint32_t add_n = (uint32_t)ceilf((want - (float)have) * fminf(dt * 5.0f, 1.0f));
        float sp = spacing_of(t, density_scale), sp2 = sp * sp, cos_max = cosf(fminf(fmaxf(t->slope, 1.0f), 89.0f) * 0.0174533f);
        for (uint32_t tries = 0; tries < add_n * 4 && add_n; tries++) {
            float a = rnd(&f->rng) * 6.2831853f, d = sqrtf(rnd(&f->rng)) * radius;
            float x = at[0] + cosf(a) * d, y = at[1] + sinf(a) * d, z, up;
            if (!ground(x, y, at[2], true, &z, &up) || up < cos_max) continue;
            bool close = false;
            for (uint32_t k = 0; k < nn && !close; k++) {
                const FolInst* o = &f->inst[F.near[k]];
                close = o->type == ty && (o->pos[0] - x) * (o->pos[0] - x) + (o->pos[1] - y) * (o->pos[1] - y) < sp2;
            }
            if (close) continue;
            FolInst in;
            place(&in, t, ty, x, y, z, &f->rng);
            if (!add(w, f, c, &in)) break;
            if (nn < 8192) F.near[nn++] = f->n - 1;
            add_n--;
        }
    }
    F.revision = w->revision;
    F.stats.edit_ms = (re_time_seconds() - t0) * 1000.0;
}

void foliage_stroke_end(World* w, Entity e) {
    (void)w;
    Fol* f = fol_of(e);
    if (f && f->save) save(f);
}

void foliage_clear(World* w, Entity e, int t) {
    Fol* f = fol_of(e);
    if (!f) return;
    for (uint32_t i = 0; i < f->n; i++)
        if (t < 0 || f->inst[i].type == (uint32_t)t) remove_at(w, f, i--);
    save(f);
    F.revision = w->revision;
}

uint32_t foliage_fill(World* w, Entity e, int t) {
    Fol* f = fol_of(e);
    CFoliage* c = ecs_get(w, e, COMP_FOLIAGE);
    float mn[2], mx[2];
    if (!f || !c || !landscape_bounds(mn, mx)) return 0;
    double t0 = re_time_seconds();
    uint32_t made = 0, rng = c->seed * 2654435761u + 12345u;
    for (uint32_t ty = 0; ty < c->ntypes; ty++) {
        const FoliageType* tp = &c->types[ty];
        if ((t >= 0 && (uint32_t)t != ty) || (t < 0 && !tp->paint)) continue;
        for (uint32_t i = 0; i < f->n; i++) // (in place of what it had)
            if (f->inst[i].type == ty) remove_at(w, f, i--);
        if (tp->density <= 0.0f) continue;
        double area_m2 = (double)(mx[0] - mn[0]) * (mx[1] - mn[1]) / 10000.0;
        uint32_t n = (uint32_t)fmin(area_m2 / 100.0 * tp->density, (double)(FOL_INSTANCES - f->n));
        float cos_max = cosf(fminf(fmaxf(tp->slope, 1.0f), 89.0f) * 0.0174533f);
        for (uint32_t k = 0; k < n * 2 && n; k++) {
            float x = mn[0] + (mx[0] - mn[0]) * rnd(&rng), y = mn[1] + (mx[1] - mn[1]) * rnd(&rng), z, up;
            if (!ground(x, y, 0.0f, false, &z, &up) || up < cos_max) continue;
            FolInst in;
            place(&in, tp, ty, x, y, z, &rng);
            if (!add(w, f, c, &in)) break;
            made++, n--;
        }
    }
    F.revision = w->revision;
    save(f);
    F.stats.edit_ms = (re_time_seconds() - t0) * 1000.0;
    return made;
}

uint32_t foliage_count(Entity e, int t) {
    Fol* f = fol_of(e);
    if (!f) return 0;
    if (t < 0) return f->n;
    uint32_t k = 0;
    for (uint32_t i = 0; i < f->n; i++) k += f->inst[i].type == (uint32_t)t;
    return k;
}

// --- Every frame ---

// An older level's landscape plants (the landscape scattered them itself until 0.14): the settings become a Foliage
// actor's type and the plants are filled again, the landscape marked moved
static void move_landscape_plants(World* w) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_LANDSCAPE)) || !ar->count) continue;
        CLandscape* l = ecs_arch_column(ar, COMP_LANDSCAPE);
        for (uint32_t r = 0; r < ar->count; r++) {
            if (l[r].foliage_moved) continue;
            l[r].foliage_moved = 1;
            if (l[r].foliage_density <= 0.0f) continue;
            Entity fe = ENTITY_NONE;
            for (uint32_t bi = 0; bi < w->archetype_count && !fe; bi++)
                if ((w->archetypes[bi].mask & COMP_BIT(COMP_FOLIAGE)) && w->archetypes[bi].count) fe = ecs_arch_entities(&w->archetypes[bi])[0];
            if (!fe) {
                fe = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_FOLIAGE));
                if (fe == ENTITY_NONE) return;
                snprintf(((CName*)ecs_get(w, fe, COMP_NAME))->text, RE_NAME_MAX, "Foliage");
                CFoliage* c = ecs_get(w, fe, COMP_FOLIAGE);
                c->ntypes = 0, c->seed = l[r].seed;
                ar = &w->archetypes[ai], l = ecs_arch_column(ar, COMP_LANDSCAPE); // (a new archetype: the columns may have moved)
            }
            CFoliage* c = ecs_get(w, fe, COMP_FOLIAGE);
            if (c->ntypes >= FOLIAGE_TYPES) continue;
            FoliageType* t = &c->types[c->ntypes++];
            memset(t, 0, sizeof *t);
            t->mesh = l[r].foliage_mesh, t->material = l[r].foliage_material, t->density = l[r].foliage_density;
            memcpy(t->color, l[r].foliage_color, 12);
            t->scale[0] = l[r].foliage_scale[0] * 0.45f, t->scale[1] = l[r].foliage_scale[1] * 0.45f, t->stretch = 3.5f; // (the old slender plants)
            t->slope = l[r].foliage_slope, t->wind = l[r].wind, t->paint = 1;
            snprintf(c->name, sizeof c->name, "@fill"); // (named and filled by the update below, its file new)
        }
    }
}

void foliage_update(World* w) {
    if (!F.root[0]) return;
    move_landscape_plants(w);
    if (w->revision != F.revision) {
        for (uint32_t i = 0; i < F.nfol; i++) F.fol[i].loaded = false;
        F.revision = w->revision;
    }
    double t0 = re_time_seconds();
    bool built = false;
    for (uint32_t i = 0; i < F.nfol; i++) F.fol[i].alive = false;
    F.stats.actors = F.stats.instances = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_FOLIAGE)) || !ar->count) continue;
        for (uint32_t r = 0; r < ar->count; r++) {
            Entity e = ecs_arch_entities(ar)[r];
            CFoliage* c = ecs_get(w, e, COMP_FOLIAGE);
            bool fill = !strcmp(c->name, "@fill"); // (a moved landscape's: filled once)
            if (fill || !c->name[0]) // (a name of its own: an older file of a deleted actor is not read back)
                snprintf(c->name, sizeof c->name, "Foliage_%08X", (uint32_t)(re_time_seconds() * 1000.0) * 2654435761u ^ e);

            if (c->ntypes > FOLIAGE_TYPES) c->ntypes = FOLIAGE_TYPES;
            Fol* f = NULL;
            for (uint32_t i = 0; i < F.nfol && !f; i++)
                if (F.fol[i].e == e) f = &F.fol[i];
            for (uint32_t i = 0; i < F.nfol && !f; i++)
                if (!strcmp(F.fol[i].name, c->name)) {
                    if (F.fol[i].alive) { // (a copy of another actor, Ctrl+D: a file of its own, empty)
                        snprintf(c->name, sizeof c->name, "Foliage_%08X", (uint32_t)(re_time_seconds() * 1000.0) * 2654435761u ^ e ^ 0x5bd1e995u);
                        break;
                    }
                    f = &F.fol[i];
                }
            if (!f) {
                if (F.nfol >= FOL_MAX) continue;
                f = &F.fol[F.nfol++];
                memset(f, 0, sizeof *f);
            }
            if (f->e != e || strcmp(f->name, c->name)) f->loaded = false; // (another actor or name: read again)
            f->e = e, f->alive = true;
            snprintf(f->name, sizeof f->name, "%s", c->name);
            ar = &w->archetypes[ai]; // (the entities below may make archetypes)
            if (!f->loaded) {
                destroy_children(w, e); // (the ones the level file kept: made again from the instances)
                f->n = 0;
                if (!fill) load(f);
                f->rng = c->seed * 747796405u + 1u;
                for (uint32_t k = 0; k < f->n; k++) f->ents[k] = make_entity(w, f, c, &f->inst[k]);
                for (uint32_t t = 0; t < FOLIAGE_TYPES; t++) f->look[t] = look_of(&c->types[t]);
                f->loaded = true, built = true;
                if (fill) foliage_fill(w, e, -1);
                c = ecs_get(w, e, COMP_FOLIAGE), ar = &w->archetypes[ai];
            }
            // a type's look changed (its mesh, material, color, wind): its entities follow
            for (uint32_t t = 0; t < c->ntypes; t++) {
                uint32_t lk = look_of(&c->types[t]);
                if (lk == f->look[t]) continue;
                f->look[t] = lk;
                for (uint32_t k = 0; k < f->n; k++)
                    if (f->inst[k].type == t) style(w, f->ents[k], &c->types[t]), ecs_touch(w, f->ents[k]);
            }
            F.stats.actors++, F.stats.instances += f->n;
        }
    }
    for (uint32_t i = 0; i < F.nfol; i++)
        if (!F.fol[i].alive && F.fol[i].e) { // (deleted: its plants with it; its file stays, like the landscape's)
            destroy_children(w, F.fol[i].e);
            F.fol[i].e = 0, F.fol[i].loaded = false, F.fol[i].n = 0, F.fol[i].name[0] = 0;
        }
    if (built) F.stats.build_ms = (re_time_seconds() - t0) * 1000.0;
    F.revision = w->revision; // (its own entities made or destroyed in bulk are no undo)
}

void foliage_retype(World* w, Entity e, int removed) {
    Fol* f = fol_of(e);
    if (!f || removed < 0 || removed >= FOLIAGE_TYPES) return;
    for (uint32_t i = 0; i < f->n; i++)
        if (f->inst[i].type > (uint32_t)removed) {
            f->inst[i].type--;
            CAttach* at = ecs_get(w, f->ents[i], COMP_ATTACH);
            if (at) at->node = f->inst[i].type;
        }
    memmove(&f->look[removed], &f->look[removed + 1], sizeof(uint32_t) * (FOLIAGE_TYPES - (uint32_t)removed - 1));
    save(f);
}
