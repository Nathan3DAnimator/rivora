// Splines in the world (spline.h): the pieces a Spline makes (Spline Mesh pieces bent on the GPU, instances along it), the
// Cables (XPBD, held by actors and sockets, colliding, in the wind) and their pieces, the views the renderer bends by, the
// Blueprint nodes' edits. The pieces are child entities (CAttach ATTACH_SPLINE) rebuilt only when their spline changed
// (a hash, gated by the world's change counter like the prototyping shapes); the views (the Hermite each piece is bent
// by) live here, never in the level. Nothing is allocated per frame: fixed tables.
#include "spline.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stddef.h>

#include "anim.h"
#include "components.h"
#include "landscape.h"
#include "platform.h"
#include "render.h"
#include "debug_viewer.h"

uint32_t bp_world_boxes(const float mn[3], const float mx[3], float (*out)[6], uint32_t cap); // (bp_vm.c: the play's boxes)

#define MAX_OWNERS 512
#define MAX_VIEWS 16384
#define MAX_CABLES 64
#define MAX_COLLIDERS 256

typedef struct Owner {
    Entity e;                 // the spline actor (0: free)
    uint32_t first, count;    // its views in g_views (mesh pieces, then a cable's segments)
    uint32_t hash;            // the views' (its curve, transform, pieces)
    uint32_t mesh, axis;      // the bent mesh (its bounds read each rebuild)
    bool cable;
    uint32_t seen;
} Owner;

typedef struct CableState {
    Entity e;
    CableSim sim;
    uint32_t key;             // segments, length: a change starts it again
    Entity ends[2];           // the resolved end actors (by name)
    char names[2][32];
    bool settled_once;
    float t;                  // its time (the gusts)
    float mn[3], mx[3];       // its bounds (the colliders' gather)
    uint32_t seen;
} CableState;

typedef struct Collider {
    float c[3], ax[3][3], h[3]; // an oriented box: center, unit axes, half extents (cm)
    bool capsule;               // a Character: c the middle, ax[2] up, h[0] radius, h[2] half height
    Entity e;                   // its actor (a component's owner): the cable's own ends don't push it
} Collider;

static struct {
    Owner owners[MAX_OWNERS];
    uint32_t nowners;
    SplineMeshView views[MAX_VIEWS];
    uint32_t nviews;
    CableState cables[MAX_CABLES];
    uint32_t ncables;
    uint32_t changes;         // the world's change counter last seen
    uint32_t frame;
    SplineStats stats;
    Collider col[MAX_COLLIDERS];
    uint32_t ncol;
    bool playing;
} S;

const SplineStats* spline_stats(void) { return &S.stats; }

static uint32_t fnv(const void* p, size_t n, uint32_t h) {
    for (size_t i = 0; i < n; i++) h = (h ^ ((const uint8_t*)p)[i]) * 16777619u;
    return h;
}
static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void cross3(const float a[3], const float b[3], float o[3]) {
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    o[0] = x, o[1] = y, o[2] = z;
}
static float norm3(float v[3]) {
    float l = sqrtf(dot3(v, v));
    if (l > 1e-12f) v[0] /= l, v[1] /= l, v[2] /= l;
    return l;
}
static float rand01(uint32_t seed, uint32_t i, uint32_t k) { // (a hash: the same instances every rebuild)
    uint32_t h = seed * 0x9E3779B1u ^ i * 0x85EBCA77u ^ k * 0xC2B2AE3Du;
    h ^= h >> 16, h *= 0x7FEB352Du, h ^= h >> 15, h *= 0x846CA68Bu, h ^= h >> 16;
    return (float)(h >> 8) / 16777216.0f;
}

// --- The curve in the world ---
bool spline_world_curve(World* w, Entity e, SplineCurve* out) {
    const CRail* r = e ? ecs_get(w, e, COMP_SPLINE) : NULL;
    const CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
    if (!r || !t) return false;
    if (spline_kind(r) == SPLINE_KIND_CABLE) { // a cable: its particles (or its rest line before it ran)
        float pts[CABLE_MAX_SEGMENTS + 1][3];
        uint32_t n = cable_points(w, e, pts, CABLE_MAX_SEGMENTS + 1);
        if (n < 2) {
            float rows[3][4];
            render_transform_rows(t, rows);
            const float z[3] = { 0, 0, 0 };
            for (int k = 0; k < 3; k++)
                pts[0][k] = rows[k][3] + rows[k][0] * z[0], pts[1][k] = rows[k][0] * r->cable_end_offset[0] + rows[k][1] * r->cable_end_offset[1] + rows[k][2] * r->cable_end_offset[2] + rows[k][3];
            n = 2;
        }
        spline_curve_points(out, (const float(*)[3])pts, n, false, NULL);
        return true;
    }
    float rows[3][4];
    render_transform_rows(t, rows);
    spline_curve_from(out, r, rows);
    return true;
}

// --- The pieces (child entities) ---
static void destroy_pieces(World* w, Entity owner) { // (owner 0: the orphans of a deleted spline)
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
        for (uint32_t r = 0; r < ar->count;) {
            const CAttach* at = &((const CAttach*)ecs_arch_column(ar, COMP_ATTACH))[r];
            if (at->kind == ATTACH_SPLINE && (owner ? at->owner == owner : !ecs_alive(w, at->owner))) ecs_destroy(w, ecs_arch_entities(ar)[r]);
            else r++;
        }
    }
}

// How many pieces each owner has (a duplicated spline copies its `built` hash but not its children: then they are made).
// Counted for all owners in one pass when the world changed (spline_update), looked up by the builds
#define PIECE_HASH 4096
static Entity g_pc_key[PIECE_HASH];
static uint32_t g_pc_val[PIECE_HASH];
static void count_all_pieces(World* w) {
    memset(g_pc_key, 0, sizeof g_pc_key);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        for (uint32_t r = 0; r < ar->count; r++) {
            if (at[r].kind != ATTACH_SPLINE || !at[r].owner) continue;
            uint32_t h = (at[r].owner * 2654435761u) & (PIECE_HASH - 1);
            for (uint32_t q = 0; q < PIECE_HASH; q++, h = (h + 1) & (PIECE_HASH - 1)) {
                if (g_pc_key[h] == at[r].owner) { g_pc_val[h]++; break; }
                if (!g_pc_key[h]) { g_pc_key[h] = at[r].owner, g_pc_val[h] = 1; break; }
            }
        }
    }
}
static uint32_t count_pieces(World* w, Entity owner) {
    (void)w;
    uint32_t h = (owner * 2654435761u) & (PIECE_HASH - 1);
    for (uint32_t q = 0; q < PIECE_HASH; q++, h = (h + 1) & (PIECE_HASH - 1)) {
        if (g_pc_key[h] == owner) return g_pc_val[h];
        if (!g_pc_key[h]) return 0;
    }
    return 0;
}

static Entity make_piece(World* w, Entity owner, uint32_t node, const char* name, uint32_t mesh, uint32_t material, const float color[3], float rough,
                         float metal, uint32_t mobility, bool bent) {
    Entity c = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
    if (c == ENTITY_NONE) return c;
    snprintf(((CName*)ecs_get(w, c, COMP_NAME))->text, RE_NAME_MAX, "%s_%u", name, node & SPLINE_NODE_INDEX);
    CAttach* at = ecs_get(w, c, COMP_ATTACH);
    at->owner = owner, at->parent = owner, at->node = node, at->kind = ATTACH_SPLINE;
    at->scale[0] = at->scale[1] = at->scale[2] = 1.0f;
    CTransform* ct = ecs_get(w, c, COMP_TRANSFORM);
    ct->scale[0] = ct->scale[1] = ct->scale[2] = 1.0f;
    ct->flags = bent ? MOBILITY_MOVABLE : mobility; // (a bent piece is a skin: the dynamic set's)
    CStaticMesh* sm = ecs_get(w, c, COMP_STATIC_MESH);
    ecs_component_defaults(COMP_STATIC_MESH, sm);
    sm->mesh = mesh, sm->material = material, sm->roughness = rough, sm->metallic = metal;
    memcpy(sm->base_color, color, 12);
    // (a bent piece's rows are its chord's: the contact AO's occluders take an instance's box from them, wrong for a curve)
    sm->cast_shadow = 1u | (bent ? CAST_SHADOW_NO_OCCLUDER : 0u);
    return c;
}

// The rows a bent piece is drawn by: its chord's frame (forward along P0 -> P1, across as the curve's middle frame) and
// its mesh's length over the chord, so the mesh's own bounds (the GPU cull's sphere, the surface cache's texels) sit
// on the piece; widened when the bend leaves that sphere. skin.comp writes the bent vertices through their inverse
static void piece_rows(const SplineMeshView* v, const float mn[3], const float mx[3], float rows[3][4]) {
    int a = (int)v->axis % 3, b = (a + 1) % 3, c = (a + 2) % 3;
    float F[3] = { v->p1[0] - v->p0[0], v->p1[1] - v->p0[1], v->p1[2] - v->p0[2] };
    float chord = norm3(F);
    if (chord < 1e-3f) {
        memcpy(F, v->t0, 12);
        if (norm3(F) < 1e-6f) F[0] = 1.0f, F[1] = F[2] = 0.0f;
        chord = 1e-3f;
    }
    float pm[3], dm[3];
    { // the middle: the Hermite at 0.5
        for (int k = 0; k < 3; k++) {
            pm[k] = 0.5f * (v->p0[k] + v->p1[k]) + 0.125f * (v->t0[k] - v->t1[k]);
            dm[k] = 1.5f * (v->p1[k] - v->p0[k]) - 0.25f * (v->t0[k] + v->t1[k]);
        }
    }
    float R[3], U[3];
    spline_frame(dm, v->up, 0.5f * (v->roll0 + v->roll1), R, U);
    float d = dot3(R, F);
    for (int k = 0; k < 3; k++) R[k] -= F[k] * d;
    if (norm3(R) < 1e-6f) spline_frame(F, v->up, 0.0f, R, U);
    cross3(F, R, U);
    float len = fmaxf(mx[a] - mn[a], 1e-3f);
    float s1 = fmaxf(fabsf(0.5f * (v->s0[0] + v->s1[0])), 1e-3f), s2 = fmaxf(fabsf(0.5f * (v->s0[1] + v->s1[1])), 1e-3f);
    float cols[3][3];
    for (int k = 0; k < 3; k++) cols[a][k] = F[k] * chord / len, cols[b][k] = R[k] * s1, cols[c][k] = U[k] * s2;
    float tr[3];
    for (int k = 0; k < 3; k++) tr[k] = v->p0[k] - cols[a][k] * v->xmin;
    // the cull's sphere (the mesh's box's) against the bent piece: samples of the curve, the cross-section around them
    float cm[3], hm[3];
    for (int k = 0; k < 3; k++) cm[k] = 0.5f * (mn[k] + mx[k]), hm[k] = 0.5f * (mx[k] - mn[k]);
    float center[3];
    for (int k = 0; k < 3; k++) center[k] = tr[k] + cols[0][k] * cm[0] + cols[1][k] * cm[1] + cols[2][k] * cm[2];
    float mcol = 0.0f;
    for (int i = 0; i < 3; i++) mcol = fmaxf(mcol, sqrtf(dot3(cols[i], cols[i])));
    float radius = sqrtf(dot3(hm, hm)) * mcol;
    float smax = fmaxf(fmaxf(fabsf(v->s0[0]), fabsf(v->s1[0])), fmaxf(fabsf(v->s0[1]), fabsf(v->s1[1])));
    float ext = sqrtf(fmaxf(fabsf(mn[b]), fabsf(mx[b])) * fmaxf(fabsf(mn[b]), fabsf(mx[b])) + fmaxf(fabsf(mn[c]), fabsf(mx[c])) * fmaxf(fabsf(mn[c]), fabsf(mx[c]))) * smax;
    float need = 0.0f;
    for (int i = 0; i <= 8; i++) {
        float t = (float)i / 8.0f, t2 = t * t, t3 = t2 * t;
        float h00 = 2 * t3 - 3 * t2 + 1, h10 = t3 - 2 * t2 + t, h01 = -2 * t3 + 3 * t2, h11 = t3 - t2, p[3];
        for (int k = 0; k < 3; k++) p[k] = h00 * v->p0[k] + h10 * v->t0[k] + h01 * v->p1[k] + h11 * v->t1[k] - center[k];
        need = fmaxf(need, sqrtf(dot3(p, p)) + ext);
    }
    (void)pm;
    if (need > radius * 0.98f && radius > 1e-6f) { // (a strong bend: the rows grown about the middle; the drawing stays exact)
        float g = need / (radius * 0.98f);
        for (int i = 0; i < 3; i++)
            for (int k = 0; k < 3; k++) cols[i][k] *= g;
        for (int k = 0; k < 3; k++) tr[k] = center[k] - (cols[0][k] * cm[0] + cols[1][k] * cm[1] + cols[2][k] * cm[2]);
    }
    for (int r = 0; r < 3; r++) {
        for (int i = 0; i < 3; i++) rows[r][i] = cols[i][r];
        rows[r][3] = tr[r];
    }
}

static uint32_t view_hash(const SplineMeshView* v) {
    uint32_t h = fnv(v, offsetof(SplineMeshView, version), 2166136261u);
    return h ? h : 1u;
}

static void place_bent(World* w, Entity c, SplineMeshView* v, uint32_t mesh) {
    float mn[3], mx[3], rows[3][4];
    render_mesh_bounds(mesh, mn, mx);
    int a = (int)v->axis % 3;
    v->xmin = mn[a], v->inv_len = 1.0f / fmaxf(mx[a] - mn[a], 1e-3f);
    v->version = view_hash(v);
    piece_rows(v, mn, mx, rows);
    CTransform* ct = ecs_get(w, c, COMP_TRANSFORM);
    if (!ct) return;
    uint32_t fl = ct->flags;
    anim_rows_transform(rows, ct); // (written raw: the dynamic set reads it each frame; no touch, nothing rebuilt)
    ct->flags = fl;
}

static Owner* owner_of(Entity e, bool make) {
    uint32_t idx = entity_index(e);
    for (uint32_t i = 0; i < S.nowners; i++)
        if (S.owners[i].e == e) return &S.owners[i];
    (void)idx;
    if (!make) return NULL;
    for (uint32_t i = 0; i < S.nowners; i++)
        if (!S.owners[i].e) return memset(&S.owners[i], 0, sizeof S.owners[i]), S.owners[i].e = e, &S.owners[i];
    if (S.nowners >= MAX_OWNERS) return NULL;
    Owner* o = &S.owners[S.nowners++];
    memset(o, 0, sizeof *o);
    o->e = e;
    return o;
}

// Room for n views (the table compacted when it ran out: the owners' ranges moved down)
static bool views_room(Owner* o, uint32_t n) {
    if (o->count >= n && o->first + n <= MAX_VIEWS) return true;
    if (S.nviews + n > MAX_VIEWS) {
        static SplineMeshView tmp[MAX_VIEWS];
        uint32_t at = 0;
        for (uint32_t i = 0; i < S.nowners; i++) {
            Owner* q = &S.owners[i];
            if (!q->e || q == o || !q->count) continue;
            memcpy(&tmp[at], &S.views[q->first], sizeof(SplineMeshView) * q->count);
            q->first = at, at += q->count;
        }
        memcpy(S.views, tmp, sizeof(SplineMeshView) * at);
        S.nviews = at;
        if (S.nviews + n > MAX_VIEWS) return false;
    }
    o->first = S.nviews, o->count = n;
    S.nviews += n;
    memset(&S.views[o->first], 0, sizeof(SplineMeshView) * n); // (no other owner's views read as this one's: a new piece is placed)
    return true;
}

bool spline_mesh_view(World* w, Entity e, SplineMeshView* out) {
    const CAttach* at = ecs_get(w, e, COMP_ATTACH);
    if (!at || at->kind != ATTACH_SPLINE || !(at->node & (SPLINE_NODE_MESH | SPLINE_NODE_CABLE))) return false;
    static uint32_t s_last;
    Owner* o = s_last < S.nowners && S.owners[s_last].e == at->owner ? &S.owners[s_last] : NULL;
    if (!o) {
        o = owner_of(at->owner, false);
        if (!o) return false;
        s_last = (uint32_t)(o - S.owners);
    }
    uint32_t i = at->node & SPLINE_NODE_INDEX;
    if (i >= o->count) return false;
    *out = S.views[o->first + i];
    return out->version != 0;
}

static uint32_t spline_hash(const CRail* r, const CTransform* t) {
    uint32_t h = fnv(&r->kind, sizeof *r - offsetof(CRail, kind), 2166136261u);
    h = fnv(r->points, sizeof(float) * 3 * (r->npoints < SPLINE_MAX_POINTS ? r->npoints : SPLINE_MAX_POINTS), h);
    h = fnv(&r->npoints, 8, h);
    h = fnv(t, sizeof(float) * 9, fnv(&t->flags, 4, h));
    return h ? h : 1u;
}

// A Spline's pieces: made again when they changed, then their views and places
static void build_spline(World* w, Entity e) {
    CRail* r = ecs_get(w, e, COMP_SPLINE);
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (!r || !t) return;
    static SplineCurve c; // (one at a time)
    float rows[3][4];
    render_transform_rows(t, rows);
    spline_curve_from(&c, r, rows);
    uint32_t h = spline_hash(r, t);
    uint32_t nseg = spline_segment_count(&c);
    uint32_t nmesh = 0, ninst = 0;
    if (r->mesh_on && nseg) {
        if (r->mesh_length > 1.0f && c.length > 1.0f) {
            float f = c.length / r->mesh_length;
            nmesh = (uint32_t)(f + 0.5f);
            if (nmesh < 1) nmesh = 1;
        } else nmesh = nseg;
        if (nmesh > SPLINE_MAX_MESHES) nmesh = SPLINE_MAX_MESHES;
    }
    if (r->inst_on && c.length > 0.0f && c.n >= 2) {
        float sp = fmaxf(r->inst_spacing, 1.0f);
        float span = c.length - fmaxf(r->inst_start, 0.0f);
        if (span >= 0.0f) ninst = (uint32_t)floorf(span / sp + 1e-3f) + (c.closed ? 0u : 1u);
        if (ninst > SPLINE_MAX_INSTANCES) ninst = SPLINE_MAX_INSTANCES;
    }
    bool rebuilt = r->built != h || count_pieces(w, e) != nmesh + ninst;
    CTransform tt = *t;
    CRail rr = *r; // (the creations move rows: copies)
    if (rebuilt) {
        double t0 = re_time_seconds();
        destroy_pieces(w, e);
        const char* nm = ((const CName*)ecs_get(w, e, COMP_NAME)) ? ((const CName*)ecs_get(w, e, COMP_NAME))->text : "Spline";
        char base[48];
        snprintf(base, sizeof base, "%.36s_Mesh", nm);
        for (uint32_t i = 0; i < nmesh; i++)
            make_piece(w, e, SPLINE_NODE_MESH | i, base, rr.mesh, rr.mesh_material, rr.mesh_color, rr.mesh_roughness, rr.mesh_metallic, MOBILITY_MOVABLE, true);
        snprintf(base, sizeof base, "%.36s_Inst", nm);
        uint32_t mob = tt.flags & TRANSFORM_MOBILITY_MASK;
        for (uint32_t i = 0; i < ninst; i++) {
            Entity p = make_piece(w, e, SPLINE_NODE_INST | i, base, rr.inst_mesh, rr.inst_material, rr.inst_color, 0.6f, 0.0f, mob, false);
            CTransform* pt = p ? ecs_get(w, p, COMP_TRANSFORM) : NULL;
            if (pt && !(rr.flags & SPLINE_F_MESH_COLLIDE)) pt->flags |= TRANSFORM_NO_COLLISION;
            if (pt) pt->flags |= tt.flags & (TRANSFORM_HIDDEN | TRANSFORM_HIDDEN_GAME);
            if (!p) break;
        }
        r = ecs_get(w, e, COMP_SPLINE);
        if (r) r->built = h;
        S.stats.rebuilt++;
        S.stats.rebuild_ms += (re_time_seconds() - t0) * 1000.0;
    }
    // the views and places (every time the world changed: a piece moved by hand goes back)
    Owner* o = owner_of(e, true);
    if (!o) return;
    o->seen = S.frame, o->cable = false, o->mesh = rr.mesh, o->axis = rr.mesh_axis;
    if (!rebuilt && o->hash == h && o->count == nmesh) return;
    o->hash = h;
    if (!views_room(o, nmesh)) {
        o->count = 0;
        return;
    }
    // the pieces by node (one pass over the children)
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        CTransform* ptr = ecs_arch_column(ar, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t k = 0; k < ar->count; k++) {
            if (at[k].kind != ATTACH_SPLINE || at[k].owner != e) continue;
            uint32_t i = at[k].node & SPLINE_NODE_INDEX;
            if ((at[k].node & SPLINE_NODE_MESH) && i < nmesh) {
                SplineMeshView* v = &S.views[o->first + i];
                memset(v, 0, sizeof *v);
                if (rr.mesh_length > 1.0f && nmesh) { // by distance: even pieces, tangents as long as the piece
                    float d0 = c.length * (float)i / (float)nmesh, d1 = c.length * (float)(i + 1) / (float)nmesh, L = d1 - d0;
                    float k0 = spline_key_at_distance(&c, d0), k1 = spline_key_at_distance(&c, d1), dir[3];
                    if (c.closed && i + 1 == nmesh) k1 = spline_key_max(&c);
                    float f0[3], f1[3], sc0[2], sc1[2];
                    spline_frame_at_key(&c, k0, v->p0, f0, NULL, NULL, sc0);
                    spline_frame_at_key(&c, k1, v->p1, f1, NULL, NULL, sc1);
                    for (int q = 0; q < 3; q++) v->t0[q] = f0[q] * L, v->t1[q] = f1[q] * L;
                    v->roll0 = spline_roll_at_key(&c, k0), v->roll1 = spline_roll_at_key(&c, k1);
                    memcpy(v->s0, sc0, 8), memcpy(v->s1, sc1, 8);
                    (void)dir;
                } else {
                    spline_segment(&c, i, v->p0, v->t0, v->p1, v->t1);
                    uint32_t i0 = i % c.n, i1 = (i + 1) % c.n;
                    v->roll0 = c.roll[i0], v->roll1 = c.roll[i1];
                    memcpy(v->s0, c.scale[i0], 8), memcpy(v->s1, c.scale[i1], 8);
                }
                memcpy(v->up, c.up, 12);
                v->axis = rr.mesh_axis % 3u;
                place_bent(w, ents[k], v, rr.mesh);
            } else if ((at[k].node & SPLINE_NODE_INST) && i < ninst) {
                float d = fmaxf(rr.inst_start, 0.0f) + fmaxf(rr.inst_spacing, 1.0f) * (float)i;
                float key = spline_key_at_distance(&c, d), p[3], f[3], rt[3], up[3];
                spline_frame_at_key(&c, key, p, f, rt, up, NULL);
                float side = rr.inst_side + rr.inst_jitter * (rand01(rr.inst_seed, i, 1) * 2.0f - 1.0f);
                float yaw = rr.inst_yaw_jitter * (rand01(rr.inst_seed, i, 2) * 2.0f - 1.0f) * 0.017453293f;
                float s = rr.inst_scale_min + (rr.inst_scale_max - rr.inst_scale_min) * rand01(rr.inst_seed, i, 3);
                float ax[3][3];
                if (rr.inst_align == SPLINE_ALIGN_FULL) memcpy(ax[0], f, 12), memcpy(ax[1], rt, 12), memcpy(ax[2], up, 12);
                else if (rr.inst_align == SPLINE_ALIGN_NONE) rotator_axes(tt.rotation, ax);
                else {
                    float fh[3] = { f[0], f[1], 0.0f };
                    if (norm3(fh) < 1e-6f) fh[0] = 1.0f;
                    memcpy(ax[0], fh, 12);
                    ax[2][0] = ax[2][1] = 0.0f, ax[2][2] = 1.0f;
                    cross3(ax[2], ax[0], ax[1]);
                }
                float cy = cosf(yaw), sy = sinf(yaw), a0[3], a1[3];
                for (int q = 0; q < 3; q++) a0[q] = ax[0][q] * cy + ax[1][q] * sy, a1[q] = ax[1][q] * cy - ax[0][q] * sy;
                memcpy(ax[0], a0, 12), memcpy(ax[1], a1, 12);
                float rws[3][4];
                for (int rr2 = 0; rr2 < 3; rr2++) {
                    for (int q = 0; q < 3; q++) rws[rr2][q] = ax[q][rr2] * s * rr.inst_scale3[q];
                    rws[rr2][3] = p[rr2] + rt[rr2] * side + ax[2][rr2] * rr.inst_up;
                }
                uint32_t fl = ptr[k].flags;
                anim_rows_transform(rws, &ptr[k]);
                ptr[k].flags = fl;
            }
        }
    }
}

// --- The cables ---
static CableState* cable_state(Entity e, bool make) {
    for (uint32_t i = 0; i < S.ncables; i++)
        if (S.cables[i].e == e) return &S.cables[i];
    if (!make) return NULL;
    for (uint32_t i = 0; i < S.ncables; i++)
        if (!S.cables[i].e) return memset(&S.cables[i], 0, sizeof S.cables[i]), S.cables[i].e = e, &S.cables[i];
    if (S.ncables >= MAX_CABLES) return NULL;
    CableState* c = &S.cables[S.ncables++];
    memset(c, 0, sizeof *c);
    c->e = e;
    return c;
}

static Entity find_named(World* w, const char* name, Entity cached) {
    if (!name || !name[0]) return ENTITY_NONE;
    if (cached && ecs_alive(w, cached)) {
        const CName* n = ecs_get(w, cached, COMP_NAME);
        if (n && !strcmp(n->text, name)) return cached;
    }
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_NAME)) || (ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
        const CName* nm = ecs_arch_column(ar, COMP_NAME);
        for (uint32_t k = 0; k < ar->count; k++)
            if (!strcmp(nm[k].text, name)) return ecs_arch_entities(ar)[k];
    }
    return ENTITY_NONE;
}

// An end's place: the actor (its socket or bone) and the offset in its space
static bool end_point(World* w, Entity self, CableState* cs, int end, const char* actor, const char* socket, const float off[3], float out[3]) {
    Entity a = self;
    if (actor && actor[0]) {
        a = find_named(w, actor, cs->ends[end]);
        cs->ends[end] = a;
        if (!a) return false;
    }
    float rows[3][4];
    bool ok = false;
    if (socket && socket[0]) ok = anim_socket_world(w, a, socket, rows);
    if (!ok) {
        const CTransform* t = ecs_get(w, a, COMP_TRANSFORM);
        if (!t) return false;
        render_transform_rows(t, rows);
    }
    for (int k = 0; k < 3; k++) out[k] = rows[k][0] * off[0] + rows[k][1] * off[1] + rows[k][2] * off[2] + rows[k][3];
    return true;
}

void cable_sim_init(CableSim* s, uint32_t segments, const float a[3], const float b[3], float len) {
    memset(s, 0, sizeof *s);
    if (segments < 1) segments = 1;
    if (segments > CABLE_MAX_SEGMENTS) segments = CABLE_MAX_SEGMENTS;
    s->n = segments + 1;
    float d[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, D = sqrtf(dot3(d, d));
    if (len < 1.0f) len = D > 1.0f ? D : 100.0f;
    s->rest = len / (float)segments;
    // a parabola of about its length (L ~ D + 8 h^2 / 3 D): it starts hanging, settles in a few frames
    float sag = len > D && D > 1.0f ? sqrtf(3.0f * D * (len - D) / 8.0f) : 0.0f;
    for (uint32_t i = 0; i < s->n; i++) {
        float t = (float)i / (float)segments;
        for (int k = 0; k < 3; k++) s->x[i][k] = a[k] + d[k] * t;
        if (D > 1.0f) s->x[i][2] -= 4.0f * sag * t * (1.0f - t);
        else s->x[i][2] -= len * t; // (both ends at one place: hanging straight down)
        memcpy(s->prev[i], s->x[i], 12);
        s->inv_mass[i] = 1.0f;
    }
}

static void solve_distance(CableSim* s, uint32_t i, uint32_t j, float rest, float alpha, float* lambda, bool unilateral) {
    float wi = s->inv_mass[i], wj = s->inv_mass[j];
    if (wi + wj <= 0.0f) return;
    float d[3] = { s->x[j][0] - s->x[i][0], s->x[j][1] - s->x[i][1], s->x[j][2] - s->x[i][2] };
    float l = sqrtf(dot3(d, d));
    if (l < 1e-6f) return;
    float C = l - rest;
    if (unilateral && C >= 0.0f) return; // (bending: only straightens, never pulls the ends in)
    float dl = (-C - alpha * *lambda) / (wi + wj + alpha);
    *lambda += dl;
    for (int k = 0; k < 3; k++) {
        float g = d[k] / l * dl;
        s->x[i][k] -= g * wi, s->x[j][k] += g * wj;
    }
}

void cable_sim_step(CableSim* s, const CableParams* p, float dt, const float* pin_a, const float* pin_b, CableCollideFn collide, void* user) {
    if (dt <= 0.0f || s->n < 2) return;
    uint32_t sub = p->substeps ? p->substeps : 1, it = p->iterations ? p->iterations : 1;
    if (sub > 16) sub = 16;
    if (it > 32) it = 32;
    float h = dt / (float)sub;
    // the compliances (cm / unit force): a stiffness of 1 is inextensible, the bending straightens a wire, barely a rope
    float st = fminf(fmaxf(p->stiffness, 0.0f), 1.0f), bd = fminf(fmaxf(p->bend, 0.0f), 1.0f);
    float alpha = (1.0f - st) * (1.0f - st) * 0.02f / (h * h), alpha_b = bd > 0.0f ? (1.0f - bd) / (bd * 50.0f + 1e-4f) * 0.02f / (h * h) : 0.0f;
    float damp = fmaxf(0.0f, 1.0f - fminf(fmaxf(p->damping, 0.0f), 50.0f) * h);
    const float g = -980.0f * p->gravity;
    s->inv_mass[0] = pin_a ? 0.0f : 1.0f;
    s->inv_mass[s->n - 1] = pin_b ? 0.0f : 1.0f;
    for (uint32_t i = 1; i + 1 < s->n; i++) s->inv_mass[i] = 1.0f;
    s->contacts = 0;
    for (uint32_t step = 0; step < sub; step++) {
        float f = (float)(step + 1) / (float)sub;
        for (uint32_t i = 0; i < s->n; i++) {
            if (s->inv_mass[i] <= 0.0f) {
                const float* pin = i == 0 ? pin_a : pin_b;
                memcpy(s->prev[i], s->x[i], 12);
                for (int k = 0; k < 3; k++) s->x[i][k] += (pin[k] - s->x[i][k]) * (1.0f / (float)(sub - step)); // (the end moves along the substeps)
                (void)f;
                continue;
            }
            float v[3];
            for (int k = 0; k < 3; k++) v[k] = (s->x[i][k] - s->prev[i][k]) / h * damp;
            v[2] += g * h;
            // the wind: a drag toward its velocity (a thin cable follows the air quickly)
            for (int k = 0; k < 3; k++) v[k] += (p->wind[k] - v[k]) * fminf(0.8f * h, 1.0f) * (dot3(p->wind, p->wind) > 0.0f ? 1.0f : 0.0f);
            memcpy(s->prev[i], s->x[i], 12);
            for (int k = 0; k < 3; k++) s->x[i][k] += v[k] * h;
        }
        memset(s->lambda, 0, sizeof s->lambda);
        float lb[CABLE_MAX_SEGMENTS + 1] = { 0 };
        for (uint32_t k = 0; k < it; k++) {
            for (uint32_t i = 0; i + 1 < s->n; i++) solve_distance(s, i, i + 1, s->rest, alpha, &s->lambda[i], false);
            if (bd > 0.0f)
                for (uint32_t i = 0; i + 2 < s->n; i++) solve_distance(s, i, i + 2, 2.0f * s->rest, alpha_b, &lb[i], true);
        }
        if (collide) // the contacts: out of the bodies, friction against the slide
            for (uint32_t i = 0; i < s->n; i++) {
                if (s->inv_mass[i] <= 0.0f) continue;
                float nrm[3];
                if (!collide(user, s->x[i], s->prev[i], p->radius, nrm)) continue;
                s->contacts++;
                float d[3] = { s->x[i][0] - s->prev[i][0], s->x[i][1] - s->prev[i][1], s->x[i][2] - s->prev[i][2] };
                float dn = dot3(d, nrm);
                for (int k = 0; k < 3; k++) s->x[i][k] -= (d[k] - nrm[k] * dn) * fminf(fmaxf(p->friction, 0.0f), 1.0f);
            }
    }
}

// The colliders near the cables this frame (once for all of them: their joint bounds)
static void gather_colliders(World* w, const float mn[3], const float mx[3]) {
    S.ncol = 0;
    for (uint32_t ai = 0; ai < w->archetype_count && S.ncol < MAX_COLLIDERS; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!ar->count || !(ar->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        bool mesh = (ar->mask & COMP_BIT(COMP_STATIC_MESH)) != 0, chr = (ar->mask & COMP_BIT(COMP_CHARACTER)) != 0;
        if (!mesh && !chr) continue;
        if (ar->mask & (COMP_BIT(COMP_WATER) | COMP_BIT(COMP_LANDSCAPE))) continue;
        const CTransform* tr = ecs_arch_column(ar, COMP_TRANSFORM);
        const CStaticMesh* sm = mesh ? ecs_arch_column(ar, COMP_STATIC_MESH) : NULL;
        const CCharacter* ch = chr ? ecs_arch_column(ar, COMP_CHARACTER) : NULL;
        const CAttach* at = (ar->mask & COMP_BIT(COMP_ATTACH)) ? ecs_arch_column(ar, COMP_ATTACH) : NULL;
        for (uint32_t k = 0; k < ar->count && S.ncol < MAX_COLLIDERS; k++) {
            if (tr[k].flags & (TRANSFORM_NO_COLLISION | TRANSFORM_HIDDEN_GAME)) continue;
            if (at && (at[k].kind == ATTACH_SPLINE || at[k].kind == ATTACH_SK_PART || at[k].kind == ATTACH_SK_SKIN || at[k].kind == ATTACH_FOLIAGE ||
                       at[k].kind == ATTACH_LANDSCAPE || at[k].kind == ATTACH_ANIM_BP))
                continue; // (the cable's own pieces; a skeleton's parts: its capsule; plants: their wind)
            Collider* c = &S.col[S.ncol];
            if (ch) {
                const float* p = tr[k].location;
                float r = ch[k].radius, hh = ch[k].half_height;
                if (p[0] + r < mn[0] || p[0] - r > mx[0] || p[1] + r < mn[1] || p[1] - r > mx[1] || p[2] + hh < mn[2] || p[2] - hh > mx[2]) continue;
                memset(c, 0, sizeof *c);
                c->e = at ? at[k].owner : ecs_arch_entities(ar)[k];
                memcpy(c->c, p, 12);
                c->ax[2][2] = 1.0f, c->h[0] = r, c->h[2] = fmaxf(hh - r, 0.0f), c->capsule = true;
                S.ncol++;
                continue;
            }
            float rows[3][4], bmn[3], bmx[3];
            render_transform_rows(&tr[k], rows);
            render_mesh_bounds(sm[k].mesh, bmn, bmx);
            float lc[3], le[3], wc[3], we[3];
            for (int q = 0; q < 3; q++) lc[q] = 0.5f * (bmn[q] + bmx[q]), le[q] = 0.5f * (bmx[q] - bmn[q]);
            for (int r = 0; r < 3; r++) {
                wc[r] = rows[r][3] + rows[r][0] * lc[0] + rows[r][1] * lc[1] + rows[r][2] * lc[2];
                we[r] = fabsf(rows[r][0]) * le[0] + fabsf(rows[r][1]) * le[1] + fabsf(rows[r][2]) * le[2];
            }
            if (wc[0] + we[0] < mn[0] || wc[0] - we[0] > mx[0] || wc[1] + we[1] < mn[1] || wc[1] - we[1] > mx[1] || wc[2] + we[2] < mn[2] || wc[2] - we[2] > mx[2]) continue;
            memset(c, 0, sizeof *c);
            c->e = at ? at[k].owner : ecs_arch_entities(ar)[k];
            memcpy(c->c, wc, 12);
            for (int a = 0; a < 3; a++) {
                float col[3] = { rows[0][a], rows[1][a], rows[2][a] };
                float l = norm3(col);
                memcpy(c->ax[a], col, 12);
                c->h[a] = le[a] * l;
            }
            if (c->h[2] < 1.0f) // (a plane, a floor: a slab 50 cm deep under its face, so what falls on it stays on top)
                for (int q = 0; q < 3; q++) c->c[q] -= c->ax[2][q] * 25.0f, c->h[2] = 25.0f;
            S.ncol++;
        }
    }
    if (S.playing && S.ncol < MAX_COLLIDERS) { // the play's static boxes (the level's collision: proto shapes, meshes)
        float bx[64][6];
        uint32_t nb = bp_world_boxes(mn, mx, bx, 64);
        for (uint32_t i = 0; i < nb && S.ncol < MAX_COLLIDERS; i++) {
            Collider* c = &S.col[S.ncol++];
            memset(c, 0, sizeof *c);
            for (int q = 0; q < 3; q++) c->c[q] = 0.5f * (bx[i][q] + bx[i][3 + q]), c->h[q] = 0.5f * (bx[i][3 + q] - bx[i][q]), c->ax[q][q] = 1.0f;
        }
    }
}

typedef struct CollideCtx { const CableState* cs; uint32_t flags; Entity self; } CollideCtx;
static bool collide_point(void* user, float p[3], const float prev[3], float radius, float nrm[3]) {
    const CollideCtx* cx = user;
    bool hit = false;
    if (cx->flags & SPLINE_COLLIDE_WORLD) { // the landscape: above its height
        float gz;
        if (landscape_height(p[0], p[1], &gz) && p[2] < gz + radius) {
            p[2] = gz + radius;
            nrm[0] = nrm[1] = 0.0f, nrm[2] = 1.0f;
            hit = true;
        }
    }
    if (!(cx->flags & (SPLINE_COLLIDE_WORLD | SPLINE_COLLIDE_BODIES))) return hit;
    for (uint32_t i = 0; i < S.ncol; i++) {
        const Collider* c = &S.col[i];
        if (c->e && (c->e == cx->self || c->e == cx->cs->ends[0] || c->e == cx->cs->ends[1])) continue; // (what it hangs from)
        float d[3] = { p[0] - c->c[0], p[1] - c->c[1], p[2] - c->c[2] };
        if (c->capsule) {
            float z = fminf(fmaxf(d[2], -c->h[2]), c->h[2]);
            float q[3] = { d[0], d[1], d[2] - z };
            float l = sqrtf(dot3(q, q)), r = c->h[0] + radius;
            if (l >= r || l < 1e-6f) continue;
            for (int k = 0; k < 3; k++) nrm[k] = q[k] / l, p[k] += nrm[k] * (r - l);
            hit = true;
            continue;
        }
        float l[3], pen = 1e30f;
        int best = -1;
        float sgn = 1.0f;
        bool inside = true;
        for (int a = 0; a < 3 && inside; a++) {
            l[a] = dot3(d, c->ax[a]);
            float e = c->h[a] + radius, o = e - fabsf(l[a]);
            if (o <= 0.0f) inside = false;
            else if (o < pen) pen = o, best = a, sgn = l[a] >= 0.0f ? 1.0f : -1.0f;
        }
        if (!inside || best < 0) continue;
        { // came in through a face (its place before was outside it): out through that one, never across the box
            float pd[3] = { prev[0] - c->c[0], prev[1] - c->c[1], prev[2] - c->c[2] }, far = 0.0f;
            for (int a = 0; a < 3; a++) {
                float lp = dot3(pd, c->ax[a]), e = c->h[a] + radius, out = fabsf(lp) - e;
                if (out > far) far = out, best = a, sgn = lp >= 0.0f ? 1.0f : -1.0f, pen = e - sgn * l[a];
            }
        }
        for (int k = 0; k < 3; k++) nrm[k] = c->ax[best][k] * sgn, p[k] += nrm[k] * pen;
        hit = true;
    }
    return hit;
}

uint32_t cable_points(World* w, Entity cable, float (*out)[3], uint32_t cap) {
    (void)w;
    CableState* cs = cable_state(cable, false);
    if (!cs || !cs->sim.n) return 0;
    uint32_t n = cs->sim.n < cap ? cs->sim.n : cap;
    memcpy(out, cs->sim.x, sizeof(float) * 3 * n);
    return n;
}

static void cable_views(World* w, Entity e, const CRail* r, CableState* cs) {
    Owner* o = owner_of(e, true);
    if (!o) return;
    o->seen = S.frame, o->cable = true;
    uint32_t n = r->cable_render == CABLE_DRAW_HIDDEN ? 0u : cs->sim.n - 1;
    if (o->count != n && !views_room(o, n)) {
        o->count = 0;
        return;
    }
    uint32_t mesh = r->cable_render == CABLE_DRAW_RIBBON ? SHAPE_PLANE : SHAPE_CYLINDER;
    o->mesh = mesh, o->axis = mesh == SHAPE_PLANE ? 0u : 2u;
    // the pieces by node: each between two particles, Catmull-Rom tangents (the joints share their frame: one smooth tube)
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t k = 0; k < ar->count; k++) {
            if (at[k].kind != ATTACH_SPLINE || at[k].owner != e || !(at[k].node & SPLINE_NODE_CABLE)) continue;
            uint32_t i = at[k].node & SPLINE_NODE_INDEX;
            if (i >= n) continue;
            SplineMeshView* v = &S.views[o->first + i];
            const float(*x)[3] = (const float(*)[3])cs->sim.x;
            uint32_t ip = i ? i - 1 : 0, i2 = i + 2 < cs->sim.n ? i + 2 : cs->sim.n - 1;
            if (v->version && v->axis == o->axis) { // (at rest: under 0.02 cm since its last bend: no new bend on the GPU)
                float d0[3] = { x[i][0] - v->p0[0], x[i][1] - v->p0[1], x[i][2] - v->p0[2] }, d1[3] = { x[i + 1][0] - v->p1[0], x[i + 1][1] - v->p1[1], x[i + 1][2] - v->p1[2] };
                float wd0 = fmaxf(r->cable_width, 0.1f) / 100.0f;
                if (dot3(d0, d0) < 4e-4f && dot3(d1, d1) < 4e-4f && fabsf(wd0 - (mesh == SHAPE_PLANE ? v->s0[0] : v->s0[1])) < 1e-6f) continue;
            }
            memcpy(v->p0, x[i], 12), memcpy(v->p1, x[i + 1], 12);
            for (int q = 0; q < 3; q++) v->t0[q] = 0.5f * (x[i + 1][q] - x[ip][q]) * (i ? 1.0f : 2.0f), v->t1[q] = 0.5f * (x[i2][q] - x[i][q]) * (i + 2 < cs->sim.n ? 1.0f : 2.0f);
            v->roll0 = v->roll1 = 0.0f;
            float wd = fmaxf(r->cable_width, 0.1f) / 100.0f;
            if (mesh == SHAPE_PLANE) v->s0[0] = v->s1[0] = wd, v->s0[1] = v->s1[1] = 1.0f;
            else v->s0[0] = v->s1[0] = v->s0[1] = v->s1[1] = wd;
            v->up[0] = v->up[1] = 0.0f, v->up[2] = 1.0f;
            v->axis = o->axis;
            place_bent(w, ents[k], v, mesh);
        }
    }
}

static void step_cable(World* w, Entity e, float dt) {
    CRail* r = ecs_get(w, e, COMP_SPLINE);
    if (!r) return;
    CableState* cs = cable_state(e, true);
    if (!cs) return;
    cs->seen = S.frame;
    float a[3], b[3];
    static const float zero[3] = { 0, 0, 0 };
    bool ha = end_point(w, e, cs, 0, r->cable_start_actor, r->cable_start_socket, r->cable_start_offset, a);
    if (!ha) end_point(w, e, cs, 0, "", "", zero, a);
    bool hb = !r->cable_end_free && end_point(w, e, cs, 1, r->cable_end_actor, r->cable_end_socket, r->cable_end_offset, b);
    if (!hb) { // a free end (or its actor gone): from this actor's offset, hanging
        end_point(w, e, cs, 1, "", "", r->cable_end_offset, b);
        if (!r->cable_end_free) hb = true; // (its actor not found: held where the offset says)
    }
    uint32_t segs = r->cable_segments < 1 ? 1 : r->cable_segments > CABLE_MAX_SEGMENTS ? CABLE_MAX_SEGMENTS : r->cable_segments;
    float len = r->cable_length;
    uint32_t key = fnv(&segs, 4, fnv(&len, 4, 2166136261u));
    if (!cs->sim.n || cs->key != key) {
        cable_sim_init(&cs->sim, segs, a, b, len);
        cs->key = key;
    }
    CableParams p = { r->cable_stiffness, r->cable_bend, r->cable_damping, r->cable_gravity, r->cable_friction, fmaxf(r->cable_width * 0.5f, 0.5f),
                      r->cable_iterations, r->cable_substeps, { 0, 0, 0 } };
    cs->t += dt;
    if (r->cable_wind != 0.0f) { // its wind: the direction, gusts by two slow sines (each cable its own phase)
        float g = 1.0f + r->cable_gusts * (0.6f * sinf(cs->t * 0.7f + (float)entity_index(e)) + 0.4f * sinf(cs->t * 2.3f + 1.7f));
        float sp = r->cable_wind * 100.0f * g, ang = r->cable_wind_dir * 0.017453293f;
        p.wind[0] = cosf(ang) * sp, p.wind[1] = sinf(ang) * sp, p.wind[2] = 0.0f;
    }
    float mn[3], mx[3];
    for (int k = 0; k < 3; k++) mn[k] = 1e30f, mx[k] = -1e30f;
    for (uint32_t i = 0; i < cs->sim.n; i++)
        for (int k = 0; k < 3; k++) mn[k] = fminf(mn[k], cs->sim.x[i][k]), mx[k] = fmaxf(mx[k], cs->sim.x[i][k]);
    for (int k = 0; k < 3; k++) cs->mn[k] = mn[k] - 50.0f - p.radius, cs->mx[k] = mx[k] + 50.0f + p.radius;
    CollideCtx cx = { cs, r->cable_collide, e };
    bool col = r->cable_collide != 0;
    if (col) { // the colliders in its box (the pieces kept to it: the gather is per cable, a few hundred boxes at most)
        gather_colliders(w, cs->mn, cs->mx);
    }
    cable_sim_step(&cs->sim, &p, dt, a, hb ? b : NULL, col ? collide_point : NULL, &cx);
    if (dt <= 0.0f) { // (paused, the editor's first frame: the ends follow at once)
        memcpy(cs->sim.x[0], a, 12);
        if (hb) memcpy(cs->sim.x[cs->sim.n - 1], b, 12);
    }
    S.stats.contacts += cs->sim.contacts;
    S.stats.particles += cs->sim.n;
}

// A Cable's pieces: made again when their count or look changed
static void build_cable_pieces(World* w, Entity e) {
    CRail* r = ecs_get(w, e, COMP_SPLINE);
    if (!r) return;
    uint32_t segs = r->cable_segments < 1 ? 1 : r->cable_segments > CABLE_MAX_SEGMENTS ? CABLE_MAX_SEGMENTS : r->cable_segments;
    uint32_t h = fnv(&segs, 4, fnv(&r->cable_render, 4, fnv(&r->cable_material, 4, fnv(r->cable_color, 16, fnv(&r->kind, 4, 2166136261u)))));
    if (!h) h = 1;
    if (r->built == h && count_pieces(w, e) == (r->cable_render == CABLE_DRAW_HIDDEN ? 0u : segs)) return;
    double t0 = re_time_seconds();
    destroy_pieces(w, e);
    CRail rr = *r;
    const CName* nm = ecs_get(w, e, COMP_NAME);
    char base[48];
    snprintf(base, sizeof base, "%.36s_Seg", nm ? nm->text : "Cable");
    if (rr.cable_render != CABLE_DRAW_HIDDEN)
        for (uint32_t i = 0; i < segs; i++)
            if (!make_piece(w, e, SPLINE_NODE_CABLE | i, base, rr.cable_render == CABLE_DRAW_RIBBON ? SHAPE_PLANE : SHAPE_CYLINDER, rr.cable_material, rr.cable_color,
                            rr.cable_roughness, 0.0f, MOBILITY_MOVABLE, true))
                break;
    r = ecs_get(w, e, COMP_SPLINE);
    if (r) r->built = h;
    Owner* o = owner_of(e, false);
    if (o) o->count = 0; // (its views made again for the new pieces)
    S.stats.rebuilt++;
    S.stats.rebuild_ms += (re_time_seconds() - t0) * 1000.0;
}

void spline_update(World* w, float dt, bool playing) {
    double t0 = re_time_seconds();
    S.frame++;
    S.playing = playing;
    uint32_t keep_rebuilt = S.stats.rebuilt;
    double keep_rms = S.stats.rebuild_ms, peak = S.stats.cable_ms_peak;
    memset(&S.stats, 0, sizeof S.stats);
    S.stats.rebuilt = keep_rebuilt, S.stats.rebuild_ms = keep_rms, S.stats.cable_ms_peak = peak;
    // the splines and cables (their entities first: the pieces' creation moves the rows)
    static Entity list[2048];
    uint32_t n = 0, ncab = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_SPLINE)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
        const CRail* r = ecs_arch_column(ar, COMP_SPLINE);
        for (uint32_t k = 0; k < ar->count && n < 2048; k++) {
            uint32_t kind = spline_kind(&r[k]);
            if (kind == SPLINE_KIND_RAIL) continue; // (vehicle.c builds a Rail's)
            list[n++] = ecs_arch_entities(ar)[k];
            S.stats.splines += kind == SPLINE_KIND_SPLINE;
            ncab += kind == SPLINE_KIND_CABLE;
        }
    }
    bool changed = w->changes != S.changes;
    if (changed) {
        count_all_pieces(w);
        for (uint32_t i = 0; i < n; i++) {
            const CRail* r = ecs_get(w, list[i], COMP_SPLINE);
            if (r && spline_kind(r) == SPLINE_KIND_SPLINE) build_spline(w, list[i]);
            else if (r) build_cable_pieces(w, list[i]);
        }
        destroy_pieces(w, 0); // (a deleted spline's pieces)
        for (uint32_t i = 0; i < S.nowners; i++) // (the owners gone: their views given back)
            if (S.owners[i].e && (!ecs_alive(w, S.owners[i].e) || !ecs_get(w, S.owners[i].e, COMP_SPLINE))) S.owners[i].e = ENTITY_NONE, S.owners[i].count = 0;
        for (uint32_t i = 0; i < S.ncables; i++)
            if (S.cables[i].e && (!ecs_alive(w, S.cables[i].e) || !ecs_get(w, S.cables[i].e, COMP_SPLINE))) S.cables[i].e = ENTITY_NONE;
        S.changes = w->changes; // (the pieces made bumped it: seen)
    }
    // the cables: simulated, their pieces placed (every frame: they move)
    double c0 = re_time_seconds();
    float step = dt > 1.0f / 30.0f ? 1.0f / 30.0f : dt;
    for (uint32_t i = 0; i < n && ncab; i++) {
        const CRail* r = ecs_get(w, list[i], COMP_SPLINE);
        if (!r || spline_kind(r) != SPLINE_KIND_CABLE) continue;
        step_cable(w, list[i], step);
        CableState* cs = cable_state(list[i], false);
        r = ecs_get(w, list[i], COMP_SPLINE);
        if (cs && r) cable_views(w, list[i], r, cs);
        S.stats.cables++;
    }
    S.stats.cable_ms = (re_time_seconds() - c0) * 1000.0;
    if (S.stats.cable_ms > S.stats.cable_ms_peak) S.stats.cable_ms_peak = S.stats.cable_ms;
    for (uint32_t i = 0; i < S.nowners; i++)
        if (S.owners[i].e && !S.owners[i].cable) S.stats.meshes += S.owners[i].count;
    S.stats.instances = 0;
    for (uint32_t ai = 0; ai < w->archetype_count && n; ai++) { // (the instances: counted from the pieces)
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        for (uint32_t k = 0; k < ar->count; k++) S.stats.instances += at[k].kind == ATTACH_SPLINE && (at[k].node & SPLINE_NODE_INST);
    }
    S.stats.update_ms = (re_time_seconds() - t0) * 1000.0;
}

void spline_begin_play(World* w) {
    (void)w;
    for (uint32_t i = 0; i < S.ncables; i++) S.cables[i].sim.n = 0; // (the cables start from their rest shape)
    S.stats.cable_ms_peak = 0.0;
}
void spline_end_play(World* w) {
    (void)w;
    for (uint32_t i = 0; i < S.ncables; i++) S.cables[i].sim.n = 0;
    S.changes = ~w->changes; // (the snapshot restored the pieces: their views again)
}

// --- The Blueprint nodes' edits ---
static CRail* spline_of(World* w, Entity e) { return e ? ecs_get(w, e, COMP_SPLINE) : NULL; }
static void to_local(World* w, Entity e, const float p[3], float out[3]) {
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    float ax[3][3], d[3] = { p[0] - t->location[0], p[1] - t->location[1], p[2] - t->location[2] };
    rotator_axes(t->rotation, ax);
    for (int a = 0; a < 3; a++) out[a] = dot3(d, ax[a]) / (fabsf(t->scale[a]) > 1e-6f ? t->scale[a] : 1e-6f);
}
bool spline_bp_add_point(World* w, Entity e, const float p[3], bool world) {
    CRail* r = spline_of(w, e);
    if (!r || r->npoints >= SPLINE_MAX_POINTS) return false;
    float lp[3];
    if (world) to_local(w, e, p, lp);
    else memcpy(lp, p, 12);
    uint32_t i = r->npoints;
    memcpy(r->points[i], lp, 12);
    memset(r->arrive[i], 0, 12), memset(r->leave[i], 0, 12);
    r->roll[i] = 0.0f, r->pscale[i][0] = r->pscale[i][1] = 1.0f, r->ptype[i] = SPLINE_POINT_CURVE;
    r->npoints++;
    ecs_touch(w, e);
    return true;
}
bool spline_bp_set_point(World* w, Entity e, int i, const float p[3], bool world) {
    CRail* r = spline_of(w, e);
    if (!r || i < 0 || (uint32_t)i >= r->npoints) return false;
    if (world) to_local(w, e, p, r->points[i]);
    else memcpy(r->points[i], p, 12);
    ecs_touch(w, e);
    return true;
}
bool spline_bp_remove_point(World* w, Entity e, int i) {
    CRail* r = spline_of(w, e);
    if (!r || i < 0 || (uint32_t)i >= r->npoints) return false;
    if (r->npoints <= 2) { // (Unreal lets it go to none: two stay, the spline needs a segment)
        return false;
    }
    spline_remove_point(r, i);
    ecs_touch(w, e);
    return true;
}
bool spline_bp_clear(World* w, Entity e) {
    CRail* r = spline_of(w, e);
    if (!r) return false;
    r->npoints = 0, r->closed = 0;
    ecs_touch(w, e);
    return true;
}
bool spline_bp_set_closed(World* w, Entity e, bool closed) {
    CRail* r = spline_of(w, e);
    if (!r) return false;
    r->closed = closed && r->npoints > 2;
    ecs_touch(w, e);
    return true;
}

bool cable_attach(World* w, Entity cable, bool end, Entity to, const char* socket) {
    CRail* r = spline_of(w, cable);
    if (!r || spline_kind(r) != SPLINE_KIND_CABLE) return false;
    const CName* n = to ? ecs_get(w, to, COMP_NAME) : NULL;
    char* an = end ? r->cable_end_actor : r->cable_start_actor;
    char* sn = end ? r->cable_end_socket : r->cable_start_socket;
    snprintf(an, 32, "%s", n && to != cable ? n->text : "");
    snprintf(sn, 32, "%s", socket ? socket : "");
    float* off = end ? r->cable_end_offset : r->cable_start_offset;
    off[0] = off[1] = off[2] = 0.0f;
    if (end) r->cable_end_free = 0;
    CableState* cs = cable_state(cable, false);
    if (cs) cs->ends[end ? 1 : 0] = to;
    return true;
}
bool cable_detach(World* w, Entity cable, bool end) {
    CRail* r = spline_of(w, cable);
    if (!r || spline_kind(r) != SPLINE_KIND_CABLE) return false;
    if (end) r->cable_end_free = 1;
    else {
        CableState* cs = cable_state(cable, false); // (the start let go: the start becomes the free end: the cable reversed)
        if (cs && cs->sim.n > 1)
            for (uint32_t i = 0; i < cs->sim.n / 2; i++) {
                float t[3];
                memcpy(t, cs->sim.x[i], 12), memcpy(cs->sim.x[i], cs->sim.x[cs->sim.n - 1 - i], 12), memcpy(cs->sim.x[cs->sim.n - 1 - i], t, 12);
                memcpy(t, cs->sim.prev[i], 12), memcpy(cs->sim.prev[i], cs->sim.prev[cs->sim.n - 1 - i], 12), memcpy(cs->sim.prev[cs->sim.n - 1 - i], t, 12);
            }
        char a[32], s[32];
        float o[3];
        memcpy(a, r->cable_end_actor, 32), memcpy(s, r->cable_end_socket, 32), memcpy(o, r->cable_end_offset, 12);
        memcpy(r->cable_start_actor, a, 32), memcpy(r->cable_start_socket, s, 32), memcpy(r->cable_start_offset, o, 12);
        r->cable_end_free = 1;
        if (cs) cs->ends[0] = cs->ends[1], cs->ends[1] = ENTITY_NONE;
    }
    return true;
}
bool cable_set_length(World* w, Entity cable, float cm) {
    CRail* r = spline_of(w, cable);
    if (!r || spline_kind(r) != SPLINE_KIND_CABLE) return false;
    r->cable_length = fmaxf(cm, 1.0f);
    CableState* cs = cable_state(cable, false);
    if (cs && cs->sim.n > 1) { // (reeled in or out where it hangs: the rest length changes, the particles stay)
        cs->sim.rest = r->cable_length / (float)(cs->sim.n - 1);
        uint32_t segs = cs->sim.n - 1;
        cs->key = fnv(&segs, 4, fnv(&r->cable_length, 4, 2166136261u));
    }
    return true;
}
bool cable_add_impulse(World* w, Entity cable, const float at[3], const float v[3], float radius) {
    (void)w;
    CableState* cs = cable_state(cable, false);
    if (!cs || !cs->sim.n) return false;
    float r2 = fmaxf(radius, 1.0f) * fmaxf(radius, 1.0f);
    bool any = false;
    for (uint32_t i = 0; i < cs->sim.n; i++) {
        float d[3] = { cs->sim.x[i][0] - at[0], cs->sim.x[i][1] - at[1], cs->sim.x[i][2] - at[2] };
        float q = dot3(d, d);
        if (q > r2) continue;
        float f = 1.0f - q / r2;
        for (int k = 0; k < 3; k++) cs->sim.prev[i][k] -= v[k] * f / 60.0f; // (a velocity change: Verlet's previous place moved back)
        any = true;
    }
    return any;
}

// --- The Debug Viewer: the update's CPU, each cable (its share of the simulation by its particles), the pieces ---
static void collect(DvList* l, const DvContext* c) {
    World* w = c->world;
    const SplineStats* st = &S.stats;
    l->ms = st->update_ms;
    snprintf(l->note, sizeof l->note, "%u splines, %u Spline Mesh pieces, %u instances, %u cables (%u particles, %u contacts); cables %.3f ms",
             st->splines, st->meshes, st->instances, st->cables, st->particles, st->contacts, st->cable_ms);
    for (uint32_t i = 0; i < S.ncables; i++) {
        const CableState* cs = &S.cables[i];
        if (!cs->e || !ecs_alive(w, cs->e) || !cs->sim.n) continue;
        const CName* n = ecs_get(w, cs->e, COMP_NAME);
        char info[48];
        snprintf(info, sizeof info, "%u particles, %u contacts", cs->sim.n, cs->sim.contacts);
        dv_add(l, n ? n->text : "Cable", info, st->particles ? (float)(st->cable_ms * cs->sim.n / st->particles) : 0.0f, cs->e);
    }
    for (uint32_t i = 0; i < S.nowners; i++) {
        const Owner* o = &S.owners[i];
        if (!o->e || o->cable || !ecs_alive(w, o->e)) continue;
        const CName* n = ecs_get(w, o->e, COMP_NAME);
        char info[48];
        snprintf(info, sizeof info, "%u Spline Mesh pieces (bent on the GPU)", o->count);
        dv_add(l, n ? n->text : "Spline", info, 0.0f, o->e);
    }
}
void spline_register_debug(void) { dv_register("Splines and Cables", collect); }
