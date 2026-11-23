// --- A skinned scene into a Skeletal Mesh, included by tlend_import.c for the glTF and FBX importers: they fill the
// bones (names, parents before children, rest = the bone's world at bind, in the Engine's space) and the triangles
// (bind pose, the Engine's space, each one's bone and material); sk_write_mesh makes the rigid parts (a part per bone
// and material, in the bone's space; over the Skeletal Mesh's room a bone's faces go with its main material, then the
// smallest bones ride their parent) and the .rskm; sk_write_anim bakes an animation from the bones' world matrices per
// frame (each bone relative to its parent); sk_actor places it. ---

typedef struct SkTri {
    float p[3][3], n[3][3], uv[3][2]; // the Engine's space at bind
    int bone, mat;                    // mat: an index of the materials' hashes (-1: none)
    float skin[3][2];                 // each corner's bones and weights (skin_pack); skinned 0: on its bone alone
    int skinned;
    uint32_t src[3];                  // each corner's source vertex in morph (read only when it has targets)
} SkTri;

typedef struct SkScene {
    char base[96];                  // the names: SK_<base>, SKP_<base>_<bone>
    int nb;
    AnimBone bones[ANIM_MAX_BONES]; // name, parent (the binds: sk_write_mesh)
    M4 rest[ANIM_MAX_BONES];
    SkTri* tris;
    size_t ntris, cap;
    MorphBuild morph;               // the morph targets (tlend_import.c): their skins carry them
} SkScene;

// A bone matrix without its scale (the Engine's bones are rigid: their sequences keep rotation and position, so a
// scaled armature, an FBX from Blender at 100, would shrink its children)
static M4 sk_unscaled(M4 m) {
    for (int c = 0; c < 3; c++) {
        float l = sqrtf(m.m[c * 4] * m.m[c * 4] + m.m[c * 4 + 1] * m.m[c * 4 + 1] + m.m[c * 4 + 2] * m.m[c * 4 + 2]);
        for (int k = 0; k < 3; k++) m.m[c * 4 + k] /= l > 1e-12f ? l : 1.0f;
    }
    return m;
}

// An animation's morph target curves (a glTF's "weights" channels) sampled per frame: n curves, v[frame * 64 + curve]
typedef struct SkCurves {
    uint32_t n;
    char name[ANIM_MAX_MORPHS][ANIM_MORPH_NAME];
    float* v;
} SkCurves;

// The parts and the Skeletal Mesh (its name into sk_name); false when nothing was written
static bool sk_write_skins(SkScene* s, const uint32_t* mhash, int nmh, const char* folder_disk, const char* folder_vpath, char* sk_name,
                           size_t name_cap, TlendReport* r);

static bool sk_write_mesh(SkScene* s, const uint32_t* mhash, int nmh, const char* folder_disk, const char* folder_vpath, char* sk_name,
                          size_t name_cap, TlendReport* r) {
    int nb = s->nb, nm = nmh + 1;
    if (nb < 1 || nb > ANIM_MAX_BONES || !s->ntris) return false;
    if (s->morph.nraw) return sk_write_skins(s, mhash, nmh, folder_disk, folder_vpath, sk_name, name_cap, r); // (morphs: skins)
    for (size_t t = 0; t < s->ntris; t++) // weighted per vertex: skins (the GPU bends them), not parts cut by bone
        if (s->tris[t].skinned) return sk_write_skins(s, mhash, nmh, folder_disk, folder_vpath, sk_name, name_cap, r);
    for (int bi = 0; bi < nb; bi++) s->rest[bi] = sk_unscaled(s->rest[bi]);
    uint32_t* bucket = calloc((size_t)nb * (size_t)nm, sizeof(uint32_t)); // faces per (bone, material + 1)
    SkelMesh* sk = calloc(1, sizeof *sk);
    TriOut* out = malloc(sizeof(TriOut) * s->ntris);
    bool ok = bucket && sk && out;
    for (size_t t = 0; ok && t < s->ntris; t++) {
        SkTri* st = &s->tris[t];
        if (st->bone < 0 || st->bone >= nb) st->bone = 0;
        if (st->mat < -1 || st->mat >= nmh) st->mat = -1;
        bucket[(size_t)st->bone * (size_t)nm + (size_t)(st->mat + 1)]++;
    }
    int nparts = 0;
    for (int k = 0; ok && k < nb * nm; k++) nparts += bucket[k] != 0;
    if (ok && nparts > ANIM_MAX_PARTS)
        for (int bi = 0; bi < nb; bi++) {
            int best = 0;
            for (int m = 1; m < nm; m++)
                if (bucket[bi * nm + m] > bucket[bi * nm + best]) best = m;
            for (int m = 0; m < nm; m++)
                if (m != best) bucket[bi * nm + best] += bucket[bi * nm + m], bucket[bi * nm + m] = 0;
            for (size_t t = 0; t < s->ntris; t++)
                if (s->tris[t].bone == bi) s->tris[t].mat = best - 1;
        }
    for (; ok;) {
        nparts = 0;
        int small = -1;
        uint32_t small_n = 0;
        for (int bi = 0; bi < nb; bi++) {
            uint32_t tot = 0;
            for (int m = 0; m < nm; m++) tot += bucket[bi * nm + m], nparts += bucket[bi * nm + m] != 0;
            if (tot && s->bones[bi].parent >= 0 && (small < 0 || tot < small_n)) small = bi, small_n = tot;
        }
        if (nparts <= ANIM_MAX_PARTS || small < 0) break;
        int p = s->bones[small].parent;
        for (int m = 0; m < nm; m++) bucket[p * nm + m] += bucket[small * nm + m], bucket[small * nm + m] = 0;
        for (size_t t = 0; t < s->ntris; t++)
            if (s->tris[t].bone == small) s->tris[t].bone = p;
    }
    char name[200], path[1100], vpath[1100], mdir[950];
    if (ok) {
        write_name(name, sizeof name, "SK_", s->base, NULL);
        snprintf(sk_name, name_cap, "%s", name);
        snprintf(sk->path, sizeof sk->path, "%s/%s.rskm", folder_disk, name);
        snprintf(sk->name, sizeof sk->name, "%s", name);
        snprintf(mdir, sizeof mdir, "%s/Meshes", folder_disk);
        re_make_dirs(mdir);
        sk->nbones = (uint32_t)nb;
        for (int bi = 0; bi < nb; bi++) {
            sk->bones[bi] = s->bones[bi];
            local_xf(s->bones[bi].parent >= 0 ? &s->rest[s->bones[bi].parent] : NULL, &s->rest[bi], &sk->bones[bi].bind);
        }
    }
    for (int bi = 0; ok && bi < nb; bi++)
        for (int m = 0; m < nm && sk->nparts < ANIM_MAX_PARTS; m++) {
            if (!bucket[bi * nm + m]) continue;
            M4 inv = m4_inv(&s->rest[bi]); // the part in its bone's space
            uint32_t nt = 0;
            for (size_t t = 0; t < s->ntris; t++) {
                if (s->tris[t].bone != bi || s->tris[t].mat != m - 1) continue;
                TriOut* o = &out[nt++];
                for (int q = 0; q < 3; q++) {
                    m4_point(&inv, s->tris[t].p[q], o->p[q]);
                    float ne[3];
                    m4_dir(&inv, s->tris[t].n[q], ne);
                    float l = v3len(ne);
                    for (int z = 0; z < 3; z++) o->n[q][z] = l > 1e-12f ? ne[z] / l : 0.0f;
                    memcpy(o->uv[q], s->tris[t].uv[q], 8);
                }
            }
            if (!nt) continue;
            char pname[200];
            write_name(pname, sizeof pname, "SKP_", s->base, s->bones[bi].name);
            if (m > 0) {
                size_t pl = strlen(pname);
                snprintf(pname + pl, sizeof pname - pl, "_%d", m - 1);
            }
            snprintf(path, sizeof path, "%s/%s.rmesh", mdir, pname);
            if (!write_rmesh(path, out, nt, false)) continue; // (the parts: no vertex colors)
            snprintf(vpath, sizeof vpath, "%s/Meshes/%s.rmesh", folder_vpath, pname);
            SkPart* p = &sk->parts[sk->nparts++];
            memset(p, 0, sizeof *p);
            p->bone = (int16_t)bi;
            p->mesh = render_mesh_id_of(vpath);
            p->local.q[3] = 1.0f, p->local.s[0] = p->local.s[1] = p->local.s[2] = 1.0f;
            p->color[0] = p->color[1] = p->color[2] = 0.7f, p->roughness = 0.5f;
            if (m > 0 && mhash) p->material = mhash[m - 1];
            r->meshes++;
        }
    if (ok) sk_stamp(sk), ok = sk->nparts > 0 && anim_mesh_save(sk);
    if (ok) r->skeletal++;
    free(bucket), free(sk), free(out);
    return ok;
}

// The skins: a skin per material, the triangles at the bind in the Skeletal Mesh's space, each corner's weights (the
// unweighted triangles, meshes held by a bone, on their bone alone)
static bool sk_write_skins(SkScene* s, const uint32_t* mhash, int nmh, const char* folder_disk, const char* folder_vpath, char* sk_name,
                           size_t name_cap, TlendReport* r) {
    int nb = s->nb, nm = nmh + 1;
    for (int bi = 0; bi < nb; bi++) s->rest[bi] = sk_unscaled(s->rest[bi]);
    SkelMesh* sk = calloc(1, sizeof *sk);
    TriOut* out = malloc(sizeof(TriOut) * s->ntris);
    float(*skw)[2] = malloc(sizeof(float[2]) * s->ntris * 3);
    bool ok = sk && out && skw;
    uint32_t* csrc = ok && morph_finish(&s->morph) ? malloc(sizeof(uint32_t) * s->ntris * 3) : NULL; // (its morphs)
    if (csrc) morph_to_skel(&s->morph, sk);
    char name[200], path[1100], vpath[1100], mdir[950];
    if (ok) {
        write_name(name, sizeof name, "SK_", s->base, NULL);
        snprintf(sk_name, name_cap, "%s", name);
        snprintf(sk->path, sizeof sk->path, "%s/%s.rskm", folder_disk, name);
        snprintf(sk->name, sizeof sk->name, "%s", name);
        snprintf(mdir, sizeof mdir, "%s/Meshes", folder_disk);
        re_make_dirs(mdir);
        sk->nbones = (uint32_t)nb;
        for (int bi = 0; bi < nb; bi++) {
            sk->bones[bi] = s->bones[bi];
            local_xf(s->bones[bi].parent >= 0 ? &s->rest[s->bones[bi].parent] : NULL, &s->rest[bi], &sk->bones[bi].bind);
        }
    }
    for (int m = 0; ok && m < nm && sk->nskins < ANIM_MAX_SKINS; m++) {
        uint32_t nt = 0;
        for (size_t t = 0; t < s->ntris; t++) {
            const SkTri* st = &s->tris[t];
            if (st->mat != m - 1) continue;
            TriOut* o = &out[nt];
            for (int q = 0; q < 3; q++) {
                memcpy(o->p[q], st->p[q], 12);
                float l = v3len(st->n[q]);
                for (int z = 0; z < 3; z++) o->n[q][z] = l > 1e-12f ? st->n[q][z] / l : 0.0f;
                memcpy(o->uv[q], st->uv[q], 8);
                if (csrc) csrc[nt * 3 + q] = st->src[q];
                if (st->skinned) memcpy(skw[nt * 3 + q], st->skin[q], 8);
                else {
                    SkinW one = { { st->bone, 0, 0, 0 }, { 1.0f, 0, 0, 0 } };
                    skin_pack(&one, st->bone, skw[nt * 3 + q]);
                }
            }
            nt++;
        }
        if (!nt) continue;
        char pname[200];
        write_name(pname, sizeof pname, "SKS_", s->base, NULL);
        size_t pl = strlen(pname);
        snprintf(pname + pl, sizeof pname - pl, "_%d", m);
        snprintf(path, sizeof path, "%s/%s.rmesh", mdir, pname);
        if (!write_skin_rmesh(path, out, (const float(*)[2])skw, nt, csrc, csrc ? &s->morph.m : NULL)) continue;
        snprintf(vpath, sizeof vpath, "%s/Meshes/%s.rmesh", folder_vpath, pname);
        SkSkin* k = &sk->skins[sk->nskins++];
        memset(k, 0, sizeof *k);
        k->mesh = render_mesh_id_of(vpath);
        k->color[0] = k->color[1] = k->color[2] = 0.7f, k->roughness = 0.5f;
        if (m > 0 && mhash) k->material = mhash[m - 1];
        r->meshes++;
    }
    if (ok) sk_stamp(sk), ok = sk->nskins > 0 && anim_mesh_save(sk);
    if (ok) r->skeletal++;
    if (ok && csrc) r->morphs += sk->nmorphs;
    free(sk), free(out), free(skw), free(csrc);
    return ok;
}

// An animation from the bones' worlds (nframes x nb, the Engine's space): A_<name>.ranim; its hash (0: not written)
static uint32_t sk_write_anim(const SkScene* s, const char* sk_name, const char* an_name, float fps, uint32_t nframes, const M4* world,
                              const char* folder_disk, const char* folder_vpath, TlendReport* r, const SkCurves* cv) {
    int nb = s->nb;
    AnimSeq* sq = calloc(1, sizeof *sq);
    float* rot = calloc((size_t)nframes * (size_t)nb * 4, sizeof(float));
    float* pos = calloc((size_t)nframes * (size_t)nb * 3, sizeof(float));
    uint32_t h = 0;
    if (sq && rot && pos && nframes >= 1) {
        char adir[950], vpath[1100];
        snprintf(adir, sizeof adir, "%s/Animations", folder_disk);
        re_make_dirs(adir);
        snprintf(sq->path, sizeof sq->path, "%s/%s.ranim", adir, an_name);
        snprintf(sq->name, sizeof sq->name, "%s", an_name);
        snprintf(sq->skeleton, sizeof sq->skeleton, "%s", sk_name);
        sq->fps = fps, sq->nframes = nframes, sq->ntracks = (uint32_t)nb, sq->loop = true;
        sq->rot = rot, sq->pos = pos;
        sq->length = (float)(nframes - 1) / fps;
        for (int bi = 0; bi < nb; bi++) snprintf(sq->track_bone[bi], ANIM_NAME, "%s", s->bones[bi].name);
        for (uint32_t fr = 0; fr < nframes; fr++)
            for (int bi = 0; bi < nb; bi++) {
                AnimXf x;
                const M4* wb = &world[(size_t)fr * (size_t)nb];
                int pb = s->bones[bi].parent;
                M4 me = sk_unscaled(wb[bi]), pe = pb >= 0 ? sk_unscaled(wb[pb]) : m4_id();
                local_xf(pb >= 0 ? &pe : NULL, &me, &x);
                memcpy(&rot[((size_t)fr * (size_t)nb + (size_t)bi) * 4], x.q, 16);
                memcpy(&pos[((size_t)fr * (size_t)nb + (size_t)bi) * 3], x.t, 12);
            }
        float* kt = cv && cv->n ? malloc(sizeof(float) * 2 * nframes) : NULL; // the morph curves: a key a frame, the held runs trimmed
        for (uint32_t ci = 0; kt && ci < cv->n; ci++) {
            uint32_t nk = 0;
            for (uint32_t fr = 0; fr < nframes; fr++) {
                float v = cv->v[(size_t)fr * ANIM_MAX_MORPHS + ci];
                float pv = fr ? cv->v[(size_t)(fr - 1) * ANIM_MAX_MORPHS + ci] : v - 1.0f;
                float nv = fr + 1 < nframes ? cv->v[(size_t)(fr + 1) * ANIM_MAX_MORPHS + ci] : v - 1.0f;
                if (fr && fr + 1 < nframes && v == pv && v == nv) continue; // (inside a still run)
                kt[nk] = (float)fr / fps, kt[nframes + nk] = v, nk++;
            }
            anim_curve_set(sq, cv->name[ci], kt, kt + nframes, nk);
        }
        free(kt);
        if (anim_seq_save(sq)) {
            r->animations++;
            snprintf(vpath, sizeof vpath, "%s/Animations/%s.ranim", folder_vpath, an_name);
            h = vhash(vpath);
        }
    }
    if (sq) free(sq->curve_keys);
    free(rot), free(pos), free(sq);
    return h;
}

// The actor: the Skeletal Mesh at the origin (the bones carry the skeleton's transform), playing an animation
static void sk_actor(World* w, const char* label, const char* sk_name, uint32_t anim, const char* folder_vpath, TlendReport* r) {
    if (!w) return;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH));
    if (e == ENTITY_NONE) return;
    char vpath[1100];
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", label);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    t->flags = MOBILITY_MOVABLE;
    CSkeletalMesh* c = ecs_get(w, e, COMP_SKELETAL_MESH);
    snprintf(vpath, sizeof vpath, "%s/%s.rskm", folder_vpath, sk_name);
    c->mesh = vhash(vpath);
    c->anim = anim, c->play_rate = 1.0f, c->loop = 1;
    r->actors++;
}
