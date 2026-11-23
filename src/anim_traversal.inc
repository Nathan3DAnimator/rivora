// (2026-11-23, notas-dev/animacao.md "Traversal") Included by anim.c after anim_warp.inc.
// Traversal like Unreal's Game Animation Sample: Try Traversal looks at what is ahead of a Character with traces (its
// front face, its top, how deep it is, the floor behind it, room for the body on it or behind it), picks the action -
// Vault (over a low, thin obstacle to the floor behind), Mantle (up onto a low one), Climb (up onto a high one) - sets
// the Motion Warping targets where the hands meet the ledge and where the feet land, and plays the action's montage:
// root motion through the obstacle (no collision while it plays, as Unreal's Flying mode), warped so the hands land on
// this ledge and the body ends on this floor whatever its height and depth.
// The clips (MM_Traversal_*) are keyed the way the get-up clips are (anim_getup.inc gk_pose: the pelvis, the spine,
// the head, and each hand and foot placed in the clip's space and solved by IK), each key with the root's place, so the
// clip carries its own root motion (the root bone walks and climbs); each one keyed against a reference obstacle (its
// front REF_D ahead of the start, REF_H high, REF_W deep), which the targets are measured from.

typedef struct TKey { float root[3]; GKey k; } TKey;
#define TK_PALMS { { -90.0f, 0.0f, 90.0f }, { 90.0f, 0.0f, -90.0f } }
#define TK_STANDH { { -6.7f, -20.1f, -0.4f }, { 6.1f, -20.1f, 3.1f } }

// The vault: a step and a push, both hands planted on the top's front edge, the hips up and the legs tucked over it,
// down past its back onto the floor, a stride and up. Reference: the front 60 ahead, 100 high, 30 deep
static const TKey g_trav_vault[] = {
    { { 0, 0, 0 }, GK_STAND(0.0f) },
    { { 18, 0, 0 }, { 0.12f, { 18, 0, 90 }, { 0, 10, 0 }, { 0, 3, 0 }, { 0, -8, 0 }, { { 38, -25, 100 }, { 38, 25, 100 } }, { { 16, -32, 118 }, { 16, 32, 118 } },
                      TK_PALMS, { { 22, -12, 8.5f }, { -4, 12, 16 } }, { { 40, -13, 50 }, { 18, 14, 46 } }, { { 0, -4 }, { 35, 4 } } } },
    { { 38, 0, 8 }, { 0.26f, { 38, 0, 100 }, { 0, 25, 0 }, { 0, 8, 0 }, { 0, -30, 0 }, { { 63, -22, 101 }, { 63, 22, 101 } }, { { 72, -42, 122 }, { 72, 42, 122 } },
                      TK_PALMS, { { 34, -12, 8.5f }, { 32, 12, 48 } }, { { 58, -13, 48 }, { 58, 15, 80 } }, { { 25, -4 }, { 20, 4 } } } },
    { { 70, 0, 35 }, { 0.45f, { 52, -4, 118 }, { -8, 42, 0 }, { 0, 6, 0 }, { 0, -40, 0 }, { { 68, -22, 106 }, { 68, 22, 106 } }, { { 80, -44, 128 }, { 80, 44, 128 } },
                       TK_PALMS, { { 64, -12, 120 }, { 62, 12, 118 } }, { { 88, -16, 140 }, { 86, 16, 138 } }, { { 30, -4 }, { 30, 4 } } } },
    { { 110, 0, 25 }, { 0.60f, { 108, 0, 116 }, { 0, 16, 0 }, { 0, 2, 0 }, { 0, -12, 0 }, { { 128, -40, 138 }, { 126, 40, 136 } }, { { 112, -46, 150 }, { 110, 46, 148 } },
                        TK_PALMS, { { 124, -12, 50 }, { 120, 12, 56 } }, { { 145, -14, 86 }, { 142, 14, 90 } }, { { 20, -4 }, { 20, 4 } } } },
    { { 145, 0, 0 }, { 0.74f, { 145, 0, 80 }, { 0, 20, 0 }, { 0, 4, 0 }, { 0, -16, 0 }, { { 166, -34, 96 }, { 164, 34, 96 } }, { { 150, -42, 120 }, { 148, 42, 120 } },
                       TK_PALMS, { { 152, -12, 8.5f }, { 141, 12, 8.5f } }, { { 178, -14, 46 }, { 168, 14, 46 } }, { { 0, -6 }, { 0, 6 } } } },
    { { 170, 0, 0 }, { 0.88f, { 168, 0, 90 }, { 0, 8, 0 }, { 0, 2, 0 }, { 0, -6, 0 }, { { 178, -27, 95 }, { 176, 27, 95 } }, { { 170, -26, 120 }, { 168, 26, 120 } },
                       TK_STANDH, { { 152, -12, 8.5f }, { 184, 12, 8.5f } }, { { 178, -12, 50 }, { 205, 13, 50 } }, { { 0, -8 }, { 0, 6 } } } },
    { { 190, 0, 0 }, GK_STAND(1.0f) },
};
// The mantle: the hands on the ledge, the right foot up onto the top, the push on the arms, the left foot up, standing
// on it. Reference: the front 50 ahead, 100 high, deep
static const TKey g_trav_mantle[] = {
    { { 0, 0, 0 }, GK_STAND(0.0f) },
    { { 20, 0, 0 }, { 0.18f, { 20, 0, 92 }, { 0, 15, 0 }, { 0, 5, 0 }, { 0, -18, 0 }, { { 50, -24, 104 }, { 50, 24, 104 } }, { { 30, -40, 125 }, { 30, 40, 125 } },
                      TK_PALMS, { { 20, -12, 8.5f }, { 12, 12, 26 } }, { { 42, -13, 50 }, { 36, 14, 58 } }, { { 0, -4 }, { 30, 4 } } } },
    { { 28, 0, 10 }, { 0.32f, { 30, 0, 108 }, { 0, 22, 0 }, { 0, 5, 0 }, { 0, -26, 0 }, { { 53, -24, 101 }, { 53, 24, 101 } }, { { 44, -44, 128 }, { 44, 44, 128 } },
                       TK_PALMS, { { 25, -12, 8.5f }, { 60, 12, 108.5f } }, { { 46, -13, 48 }, { 58, 16, 150 } }, { { 30, -4 }, { 0, 4 } } } },
    { { 40, 0, 12 }, { 0.50f, { 42, 0, 112 }, { 0, 14, 0 }, { 0, 3, 0 }, { 0, -14, 0 }, { { 55, -24, 101 }, { 55, 24, 101 } }, { { 46, -46, 128 }, { 46, 46, 128 } },
                       TK_PALMS, { { 26, -12, 36 }, { 62, 12, 108.5f } }, { { 46, -14, 62 }, { 66, 15, 158 } }, { { 40, -4 }, { 0, 4 } } } },
    { { 62, 0, 55 }, { 0.66f, { 64, 0, 156 }, { 0, 16, 0 }, { 0, 3, 0 }, { 0, -10, 0 }, { { 76, -28, 136 }, { 76, 28, 136 } }, { { 62, -40, 152 }, { 62, 40, 152 } },
                       TK_STANDH, { { 70, -12, 122 }, { 63, 12, 108.5f } }, { { 88, -14, 146 }, { 84, 13, 150 } }, { { 20, -4 }, { 0, 4 } } } },
    { { 85, 0, 90 }, { 0.82f, { 84, 0, 188 }, { 0, 6, 0 }, { 0, 1, 0 }, { 0, -4, 0 }, { { 92, -27, 192 }, { 90, 27, 192 } }, { { 86, -24, 216 }, { 84, 24, 216 } },
                       TK_STANDH, { { 88, -12, 108.5f }, { 72, 12, 108.5f } }, { { 108, -10, 150 }, { 94, 13, 150 } }, { { 0, -8 }, { 0, 6 } } } },
    { { 95, 0, 100 }, GK_STAND(1.0f) },
};
// The climb: crouch, jump and catch the ledge, hang a moment with the feet on the wall, pull up walking the wall, press
// over the edge on the arms, the right knee up onto the top, step up and stand. Reference: the front 45 ahead, 180 high
static const TKey g_trav_climb[] = {
    { { 0, 0, 0 }, GK_STAND(0.0f) },
    { { 5, 0, 0 }, { 0.12f, { 6, 0, 78 }, { 0, 18, 0 }, { 0, 6, 0 }, { 0, -20, 0 }, { { -8, -28, 82 }, { -8, 28, 82 } }, { { 6, -32, 110 }, { 6, 32, 110 } },
                     TK_STANDH, { { 6, -12, 8.5f }, { -2, 12, 8.5f } }, { { 34, -13, 46 }, { 26, 14, 46 } }, { { 0, -6 }, { 0, 6 } } } },
    { { 22, 0, 25 }, { 0.26f, { 25, 0, 118 }, { 0, 8, 0 }, { 0, 0, 0 }, { 0, -25, 0 }, { { 40, -24, 196 }, { 40, 24, 196 } }, { { 34, -42, 168 }, { 34, 42, 168 } },
                       TK_PALMS, { { 18, -12, 32 }, { 20, 12, 30 } }, { { 36, -13, 72 }, { 38, 14, 70 } }, { { 45, -4 }, { 45, 4 } } } },
    { { 30, 0, 15 }, { 0.36f, { 30, 0, 100 }, { 0, 4, 0 }, { 0, 0, 0 }, { 0, -22, 0 }, { { 48, -24, 182 }, { 48, 24, 182 } }, { { 40, -44, 156 }, { 40, 44, 156 } },
                       TK_PALMS, { { 34, -12, 52 }, { 33, 12, 56 } }, { { 44, -14, 82 }, { 44, 14, 84 } }, { { -40, -4 }, { -40, 4 } } } },
    { { 30, 0, 0 }, { 0.46f, { 28, 0, 88 }, { 0, -4, 0 }, { 0, 0, 0 }, { 0, -18, 0 }, { { 48, -24, 182 }, { 48, 24, 182 } }, { { 40, -42, 152 }, { 40, 42, 152 } },
                      TK_PALMS, { { 33, -12, 50 }, { 34, 12, 62 } }, { { 44, -14, 76 }, { 44, 14, 86 } }, { { -60, -4 }, { -60, 4 } } } },
    { { 38, 0, 50 }, { 0.62f, { 35, 0, 138 }, { 0, 10, 0 }, { 0, 2, 0 }, { 0, -10, 0 }, { { 48, -24, 182 }, { 48, 24, 182 } }, { { 24, -36, 188 }, { 24, 36, 188 } },
                       TK_PALMS, { { 36, -12, 96 }, { 35, 12, 116 } }, { { 45, -14, 124 }, { 45, 14, 142 } }, { { -50, -4 }, { -50, 4 } } } },
    { { 52, 0, 90 }, { 0.74f, { 50, 0, 176 }, { 0, 35, 0 }, { 0, 8, 0 }, { 0, -40, 0 }, { { 56, -24, 181 }, { 56, 24, 181 } }, { { 42, -36, 204 }, { 42, 36, 204 } },
                       TK_PALMS, { { 40, -12, 122 }, { 62, 12, 191 } }, { { 46, -14, 150 }, { 86, 15, 214 } }, { { -20, -4 }, { 10, 4 } } } },
    { { 72, 0, 145 }, { 0.86f, { 74, 0, 228 }, { 0, 14, 0 }, { 0, 3, 0 }, { 0, -10, 0 }, { { 88, -28, 214 }, { 88, 28, 214 } }, { { 74, -40, 238 }, { 74, 40, 238 } },
                        TK_STANDH, { { 62, -12, 200 }, { 68, 12, 188.5f } }, { { 86, -14, 226 }, { 92, 13, 230 } }, { { 20, -4 }, { 0, 4 } } } },
    { { 85, 0, 180 }, GK_STAND(1.0f) },
};

typedef struct TravClip {
    const char* clip; const char* montage;
    const TKey* keys; int n;
    float len;
    float ref_d, ref_h, ref_w; // the reference obstacle: its front ahead of the start, its height, its depth (vault)
    float plant_u;             // the hands meet the ledge (FrontLedge's window ends)
    float land_u;              // the second window (BackFloor / TopFloor) begins
    const char* land_target;
} TravClip;
enum { TRAV_NONE = 0, TRAV_VAULT, TRAV_MANTLE, TRAV_CLIMB, TRAV_COUNT };
static const char* const g_trav_names[TRAV_COUNT] = { "None", "Vault", "Mantle", "Climb" };
static const TravClip g_trav[TRAV_COUNT] = {
    { 0 },
    { "MM_Traversal_Vault", "AM_Traversal_Vault", g_trav_vault, (int)(sizeof g_trav_vault / sizeof g_trav_vault[0]), 1.15f, 60, 100, 30, 0.26f, 0.45f, "BackFloor" },
    { "MM_Traversal_Mantle", "AM_Traversal_Mantle", g_trav_mantle, (int)(sizeof g_trav_mantle / sizeof g_trav_mantle[0]), 1.25f, 50, 100, 0, 0.32f, 0.50f, "TopFloor" },
    { "MM_Traversal_Climb", "AM_Traversal_Climb", g_trav_climb, (int)(sizeof g_trav_climb / sizeof g_trav_climb[0]), 1.85f, 45, 180, 0, 0.36f, 0.62f, "TopFloor" },
};

// The root's place at u (the same easing gk_pose uses between its keys: a hand keyed at one place stays there)
static void trav_root_at(const TKey* k, int n, float u, float out[3]) {
    int i = 0;
    while (i + 1 < n && k[i + 1].k.u <= u) i++;
    int j = i + 1 < n ? i + 1 : i;
    float s = j != i && k[j].k.u > k[i].k.u ? smooth01((u - k[i].k.u) / (k[j].k.u - k[i].k.u)) : 0.0f;
    for (int a = 0; a < 3; a++) out[a] = lerpf(k[i].root[a], k[j].root[a], s);
}
// The keys in the root's space (each place less that key's root; the last, the idle's first pose, is already in it)
static void trav_local(const TKey* k, int n, GKey* out) {
    for (int i = 0; i < n; i++) {
        out[i] = k[i].k;
        if (i == n - 1) continue;
        const float* r = k[i].root;
        for (int a = 0; a < 3; a++) {
            out[i].pel[a] -= r[a];
            for (int s = 0; s < 2; s++) out[i].hand[s][a] -= r[a], out[i].elbow[s][a] -= r[a], out[i].ank[s][a] -= r[a], out[i].knee[s][a] -= r[a];
        }
    }
}
static AnimSeq* trav_make_seq(const SkelMesh* m, const char* path, const char* vpath, const TravClip* c) {
    uint32_t nf = (uint32_t)lroundf(c->len * 30.0f) + 1;
    AnimSeq* s = seq_alloc(nf, MB_COUNT);
    GKey loc[16];
    if (!s || c->n > 16) return s;
    trav_local(c->keys, c->n, loc);
    snprintf(s->path, sizeof s->path, "%s", path);
    name_of(path, s->name);
    s->hash = vpath_hash(vpath);
    snprintf(s->skeleton, sizeof s->skeleton, "%s", m->name);
    s->fps = 30.0f, s->length = c->len, s->loop = false, s->generator = ANIM_GENERATOR;
    for (int b = 0; b < MB_COUNT; b++) snprintf(s->track_bone[b], ANIM_NAME, "%s", m->bones[b].name);
    for (uint32_t f = 0; f < nf; f++) {
        float u = fminf((float)f / 30.0f / c->len, 1.0f), root[3];
        QPose P;
        gk_pose(loc, c->n, u, &P);
        trav_root_at(c->keys, c->n, u, root);
        for (int b = 0; b < MB_COUNT; b++) {
            q_norm(P.q[b]);
            memcpy(&s->rot[((size_t)f * MB_COUNT + (uint32_t)b) * 4], P.q[b], 16);
            memcpy(&s->pos[((size_t)f * MB_COUNT + (uint32_t)b) * 3], m->bones[b].bind.t, 12);
        }
        memcpy(&s->pos[((size_t)f * MB_COUNT + MB_PELVIS) * 3], P.pelvis, 12);
        memcpy(&s->pos[((size_t)f * MB_COUNT + MB_ROOT) * 3], root, 12); // (the root walks: its root motion)
    }
    return s;
}

// The traversal clips and montages in <Mannequin>/Traversal, made when missing (or by an older generation)
static uint32_t traversal_pack(const SkelMesh* m, const char* dir) {
    char tdir[800], path[900], vpath[900];
    snprintf(tdir, sizeof tdir, "%s/Traversal", dir);
    uint32_t made = 0;
    for (int a = 1; a < TRAV_COUNT; a++) {
        const TravClip* c = &g_trav[a];
        if (!re_file_stat(tdir, NULL, NULL, NULL)) re_make_dirs(tdir);
        snprintf(path, sizeof path, "%s/%s.ranim", tdir, c->clip);
        vpath_of(path, vpath, sizeof vpath);
        AnimSeq* old = re_file_stat(path, NULL, NULL, NULL) ? anim_seq_load(vpath) : NULL;
        if (!old || old->generator < ANIM_GENERATOR) {
            AnimSeq* s = trav_make_seq(m, path, vpath, c);
            if (s) {
                made += anim_seq_save(s);
                if (old) { // (the loaded one takes the new frames)
                    free(old->rot), free(old->pos);
                    old->rot = s->rot, old->pos = s->pos, old->nframes = s->nframes, old->ntracks = s->ntracks, old->length = s->length;
                    old->generator = s->generator, old->map_for = NULL;
                    memcpy(old->track_bone, s->track_bone, sizeof old->track_bone);
                    free(s);
                } else
                    free(s->rot), free(s->pos), free(s);
            }
        }
        snprintf(path, sizeof path, "%s/%s.rmontage", tdir, c->montage);
        if (re_file_stat(path, NULL, NULL, NULL)) continue;
        AnimMontage mo = { 0 };
        snprintf(mo.path, sizeof mo.path, "%s", path);
        snprintf(mo.anim, ANIM_NAME, "%s", c->clip);
        snprintf(mo.slot, ANIM_NAME, "DefaultSlot");
        mo.blend_in = 0.12f, mo.blend_out = 0.2f, mo.rate = 1.0f, mo.root_motion = true, mo.no_collision = true;
        snprintf(mo.sections[0].name, ANIM_NAME, "Default");
        mo.nsections = 1, mo.nwarps = 2;
        snprintf(mo.warps[0].target, ANIM_NAME, "FrontLedge");
        mo.warps[0].start = 0.0f, mo.warps[0].end = c->plant_u * c->len, mo.warps[0].flags = ANIM_WARP_TRANSLATION | ANIM_WARP_ROTATION;
        snprintf(mo.warps[1].target, ANIM_NAME, "%s", c->land_target);
        mo.warps[1].start = c->land_u * c->len, mo.warps[1].end = c->len, mo.warps[1].flags = ANIM_WARP_TRANSLATION;
        made += anim_montage_save(&mo);
    }
    return made;
}

// --- Try Traversal ---

static struct { double ms; uint32_t checks; } g_trav_cost;
typedef struct TravCheck {
    int action;
    float ground_z, top_z, back_z;   // the floor here, the obstacle's top, the floor behind it
    float front[3], back[3];         // its front face's point at the top (where the trace met it), its back edge
    float fw[2];                     // into the obstacle (its face's normal turned around)
    float height, depth;
} TravCheck;

static bool trav_ray(World* w, Entity self, const float a[3], const float b[3], SqHit* h) {
    SqQuery q;
    memset(&q, 0, sizeof q);
    memcpy(q.start, a, 12), memcpy(q.end, b, 12);
    q.shape = SQ_LINE, q.nignore = 1, q.ignore = &self;
    return sq_trace(w, &q, h);
}
// room for the capsule standing with its center at c
static bool trav_room(World* w, Entity self, const float c[3], float radius, float half) {
    SqQuery q;
    SqHit h;
    memset(&q, 0, sizeof q);
    q.start[0] = q.end[0] = c[0], q.start[1] = q.end[1] = c[1], q.start[2] = c[2] + 2.0f, q.end[2] = c[2] + 3.0f;
    q.shape = SQ_CAPSULE, q.radius = radius * 0.9f, q.half[2] = half - 2.0f, q.nignore = 1, q.ignore = &self;
    return !sq_trace(w, &q, &h);
}

static void trav_check(World* w, Entity e, float max_dist, float max_h, bool draw, TravCheck* r) {
    memset(r, 0, sizeof *r);
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    const CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    if (!t || !c) return;
    float hh = c->half_height > 1.0f ? c->half_height : 88.0f, rad = c->radius > 1.0f ? c->radius : 34.0f;
    float yaw = t->rotation[1] * DEG2RAD, f[2] = { cosf(yaw), sinf(yaw) };
    const float* L = t->location;
    SqHit h;
    uint32_t red = dd_rgba(1, 0.2f, 0.2f, 1), grn = dd_rgba(0.2f, 1, 0.3f, 1), yel = dd_rgba(1, 0.85f, 0.2f, 1);
    // the floor under it
    float a[3] = { L[0], L[1], L[2] }, b[3] = { L[0], L[1], L[2] - hh - 60.0f };
    r->ground_z = trav_ray(w, e, a, b, &h) ? h.impact_point[2] : L[2] - hh;
    float standoff = L[2] - r->ground_z;
    // its front: lines ahead at several heights, the nearest wall (a face that stands)
    float best = 1e30f, N[3] = { 0, 0, 0 };
    static const float heights[] = { 35, 60, 95, 130, 170, 210 };
    for (size_t k = 0; k < sizeof heights / sizeof heights[0]; k++) {
        if (heights[k] > max_h + 10.0f) break;
        float s[3] = { L[0], L[1], r->ground_z + heights[k] }, en[3] = { L[0] + f[0] * (max_dist + rad), L[1] + f[1] * (max_dist + rad), s[2] };
        bool hit = trav_ray(w, e, s, en, &h);
        if (draw) dd_line(s, hit ? h.impact_point : en, hit ? red : grn, 1.0f, 2.0f);
        if (hit && fabsf(h.impact_normal[2]) < 0.5f && h.distance < best) best = h.distance, memcpy(r->front, h.impact_point, 12), memcpy(N, h.impact_normal, 12);
    }
    if (best > 1e29f) return;
    float nl = hypotf(N[0], N[1]);
    if (nl < 1e-3f) return;
    r->fw[0] = -N[0] / nl, r->fw[1] = -N[1] / nl;
    // its top: down from above, a little into it
    float tp[3] = { r->front[0] + r->fw[0] * 6.0f, r->front[1] + r->fw[1] * 6.0f, r->ground_z + max_h + 40.0f };
    float tb[3] = { tp[0], tp[1], r->ground_z + 20.0f };
    if (!trav_ray(w, e, tp, tb, &h) || h.impact_normal[2] < 0.7f || h.start_penetrating) return; // (too high: no top in reach)
    r->top_z = h.impact_point[2], r->front[2] = r->top_z;
    r->height = r->top_z - r->ground_z;
    if (draw) dd_point(r->front, yel, 10.0f, 2.0f);
    if (r->height < 30.0f || r->height > max_h) return; // (a step: walked; out of reach)
    // its depth: down at steps into it until the top ends (or drops)
    r->depth = 1e30f;
    for (float d = 10.0f; d <= 150.0f; d += 10.0f) {
        float p[3] = { r->front[0] + r->fw[0] * d, r->front[1] + r->fw[1] * d, r->top_z + 30.0f }, q[3] = { p[0], p[1], r->top_z - 15.0f };
        if (!trav_ray(w, e, p, q, &h) || h.impact_point[2] < r->top_z - 12.0f) {
            r->depth = d - 5.0f;
            break;
        }
    }
    if (r->depth < 1e29f) {
        r->back[0] = r->front[0] + r->fw[0] * r->depth, r->back[1] = r->front[1] + r->fw[1] * r->depth, r->back[2] = r->top_z;
        float p[3] = { r->back[0] + r->fw[0] * 45.0f, r->back[1] + r->fw[1] * 45.0f, r->top_z + 10.0f }, q[3] = { p[0], p[1], r->top_z - 400.0f };
        r->back_z = trav_ray(w, e, p, q, &h) ? h.impact_point[2] : -1e30f;
        if (draw) dd_point(r->back, yel, 10.0f, 2.0f);
    }
    // the action: over it (thin, the floor well below its top behind it, room there), onto it (room on it), else none
    const TravClip* V = &g_trav[TRAV_VAULT];
    float land[3] = { 0, 0, 0 }, top[3];
    if (r->height <= 130.0f && r->depth <= 90.0f && r->back_z > -1e29f && r->back_z < r->top_z - 40.0f) {
        float ex = V->keys[V->n - 1].root[0] - (V->ref_d + V->ref_w);
        land[0] = r->back[0] + r->fw[0] * ex, land[1] = r->back[1] + r->fw[1] * ex, land[2] = r->back_z + standoff;
        if (trav_room(w, e, land, rad, hh)) r->action = TRAV_VAULT;
    }
    if (!r->action && (r->depth >= 50.0f)) {
        int act = r->height <= 130.0f ? TRAV_MANTLE : TRAV_CLIMB;
        const TravClip* C = &g_trav[act];
        float ex = C->keys[C->n - 1].root[0] - C->ref_d;
        top[0] = r->front[0] + r->fw[0] * ex, top[1] = r->front[1] + r->fw[1] * ex, top[2] = r->top_z + standoff;
        if (trav_room(w, e, top, rad, hh)) r->action = act;
    }
    if (draw && r->action) dd_capsule(r->action == TRAV_VAULT ? land : top, hh, rad, grn, 1.0f, 2.0f);
}

// The check, its targets set and its montage played: the action (TRAV_NONE: nothing done)
int anim_try_traversal(World* w, Entity e, float max_dist, float max_h, bool draw, float* height) {
    double t0 = re_time_seconds();
    TravCheck r;
    trav_check(w, e, max_dist > 1.0f ? max_dist : 150.0f, max_h > 1.0f ? max_h : 230.0f, draw, &r);
    if (height) *height = r.height;
    const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (r.action && t) {
        const TravClip* c = &g_trav[r.action];
        float standoff = t->location[2] - r.ground_z, plant[3], face = atan2f(r.fw[1], r.fw[0]) / DEG2RAD;
        trav_root_at(c->keys, c->n, c->plant_u, plant);
        float lx = plant[0] - c->ref_d, end[3];
        float pl[3] = { r.front[0] + r.fw[0] * lx, r.front[1] + r.fw[1] * lx, r.top_z + (plant[2] - c->ref_h) + standoff };
        memcpy(end, c->keys[c->n - 1].root, 12);
        float ld[3];
        if (r.action == TRAV_VAULT) {
            float ex = end[0] - (c->ref_d + c->ref_w);
            ld[0] = r.back[0] + r.fw[0] * ex, ld[1] = r.back[1] + r.fw[1] * ex, ld[2] = r.back_z + standoff;
        } else {
            float ex = end[0] - c->ref_d;
            ld[0] = r.front[0] + r.fw[0] * ex, ld[1] = r.front[1] + r.fw[1] * ex, ld[2] = r.top_z + standoff;
        }
        anim_warp_target_set(w, e, "FrontLedge", pl, face);
        anim_warp_target_set(w, e, c->land_target, ld, face);
        if (!anim_play_montage(w, e, c->montage, 1.0f)) r.action = TRAV_NONE;
    }
    g_trav_cost.ms += (re_time_seconds() - t0) * 1000.0, g_trav_cost.checks++;
    return r.action;
}
const char* anim_traversal_name(int action) { return action >= 0 && action < TRAV_COUNT ? g_trav_names[action] : "None"; }
void anim_traversal_cost(double* ms, uint32_t* checks) {
    if (ms) *ms = g_trav_cost.ms;
    if (checks) *checks = g_trav_cost.checks;
}

// (2026-11-23) Which pair to play: a paired montage's fit to where the partner is now (its place and facing against the
// montage's partner place from the primary at its start); -1: out of the limits. Can it play: that fit within them and
// nothing between the two (a line at chest height, both left out)
static float pair_fit(World* w, Entity primary, Entity partner, const AnimMontage* mp, float max_dist, float max_angle) {
    const CTransform* pt = ecs_get(w, primary, COMP_TRANSFORM);
    const CTransform* st = ecs_get(w, partner, COMP_TRANSFORM);
    if (!pt || !st || !mp || !mp->partner[0]) return -1.0f;
    float y = pt->rotation[1] * DEG2RAD, c = cosf(y), s = sinf(y), ox = mp->partner_offset[0], oy = mp->partner_offset[1];
    float px = pt->location[0] + c * ox - s * oy, py = pt->location[1] + s * ox + c * oy;
    float d = hypotf(st->location[0] - px, st->location[1] - py);
    float a = fabsf(remainderf(st->rotation[1] - (pt->rotation[1] + mp->partner_yaw), 360.0f));
    if ((max_dist > 0.0f && d > max_dist) || (max_angle > 0.0f && a > max_angle)) return -1.0f;
    return d + a * 2.0f; // (a degree weighs as 2 cm)
}
const char* anim_find_paired(World* w, Entity primary, Entity partner, const char* prefix, float max_dist, float max_angle) {
    const char* best = NULL;
    float bs = 1e30f;
    size_t pl = prefix ? strlen(prefix) : 0;
    for (uint32_t i = 0; i < anim_montage_count(); i++) {
        const AnimMontage* m = anim_montage_at(i);
        if (!m || !m->partner[0] || (pl && _strnicmp(m->name, prefix, pl))) continue;
        float f = pair_fit(w, primary, partner, m, max_dist, max_angle);
        if (f >= 0.0f && f < bs) bs = f, best = m->name;
    }
    return best;
}
bool anim_can_play_paired(World* w, Entity primary, Entity partner, const char* montage, float max_dist, float max_angle) {
    const AnimMontage* m = montage ? anim_montage_name(montage) : NULL;
    if (!m || primary == partner || pair_fit(w, primary, partner, m, max_dist, max_angle) < 0.0f) return false;
    const CTransform* pt = ecs_get(w, primary, COMP_TRANSFORM);
    const CTransform* st = ecs_get(w, partner, COMP_TRANSFORM);
    SqQuery q;
    SqHit h;
    memset(&q, 0, sizeof q);
    Entity ig[2] = { primary, partner };
    for (int k = 0; k < 3; k++) q.start[k] = pt->location[k], q.end[k] = st->location[k];
    q.start[2] += 30.0f, q.end[2] += 30.0f; // (about the chest)
    q.shape = SQ_LINE, q.nignore = 2, q.ignore = ig;
    return !sq_trace(w, &q, &h);
}
