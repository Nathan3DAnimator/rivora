// --- Auto Rig (humanoid), included by tlend_import.c after import_skin.inc: a Static Mesh of a person becomes a
// Skeletal Mesh on the Mannequin's skeleton, so every Mannequin animation (and the packs, Motion Matching) plays on it.
//   * fitted by its height: the mesh is scaled to the Mannequin's (the actor gets the inverse scale back), its feet on
//     the ground, its middle on the axis; it faces +X like the Mannequin (the importers turn models that way)
//   * a T-pose is found by its width (arms out: wider than 0.65 of its height); its arms are laid down along the body
//     (each arm's triangles turned about its shoulder), the Mannequin's rest pose
//   * each triangle goes to the bone whose segment (joint to child joint; hands, feet and the head: a short piece past
//     them) is nearest its middle: rigid parts per bone (segmented skinning, as every Skeletal Mesh here) ---

// The Mannequin's bones the importer knows by name (anim.c makes it: these names, this order)
static int ar_bone(const SkelMesh* m, const char* name) {
    for (uint32_t b = 0; b < m->nbones; b++)
        if (!strcmp(m->bones[b].name, name)) return (int)b;
    return -1;
}

static float ar_seg_dist(const float q[3], const float a[3], const float b[3]) {
    float ab[3] = { b[0] - a[0], b[1] - a[1], b[2] - a[2] }, aq[3] = { q[0] - a[0], q[1] - a[1], q[2] - a[2] };
    float l2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2], t = l2 > 1e-6f ? (aq[0] * ab[0] + aq[1] * ab[1] + aq[2] * ab[2]) / l2 : 0.0f;
    t = t < 0.0f ? 0.0f : t > 1.0f ? 1.0f : t;
    float d[3] = { aq[0] - ab[0] * t, aq[1] - ab[1] * t, aq[2] - ab[2] * t };
    return sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
}

bool autorig_humanoid(World* w, Entity actor, const char* folder_vpath, const char* content_root, TlendReport* r) {
    memset(r, 0, sizeof *r);
    const CStaticMesh* sm = ecs_get(w, actor, COMP_STATIC_MESH);
    const CTransform* at = ecs_get(w, actor, COMP_TRANSFORM);
    const SkelMesh* man = anim_mesh_name("SK_Mannequin");
    if (!sm || !at || !man) return snprintf(r->text, sizeof r->text, "%s", !man ? "the Mannequin is missing" : "not a Static Mesh actor"), false;
    const GpuVertex* v;
    const uint32_t* idx;
    uint32_t fi, ic;
    int32_t vo;
    render_mesh_cpu(sm->mesh, &v, &idx, &fi, &ic, &vo);
    size_t ntri = ic / 3;
    if (ntri < 12 || !v || !idx) return snprintf(r->text, sizeof r->text, "the mesh has too few triangles"), false;
    // the bounds (the actor's scale applied), the height against the Mannequin's
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t k = 0; k < ic; k++) {
        const float* p = v[(int32_t)idx[fi + k] + vo].pos;
        for (int a = 0; a < 3; a++) mn[a] = fminf(mn[a], p[a] * at->scale[a]), mx[a] = fmaxf(mx[a], p[a] * at->scale[a]);
    }
    int head = ar_bone(man, "head"), pelvis = ar_bone(man, "pelvis");
    if (head < 0 || pelvis < 0) return snprintf(r->text, sizeof r->text, "the Mannequin's skeleton is not the expected one"), false;
    float man_h = man->bind_world[head][2][3] + 24.0f, H = mx[2] - mn[2];
    if (H < 1.0f) return snprintf(r->text, sizeof r->text, "the mesh is flat"), false;
    float s = man_h / H, c[3] = { (mn[0] + mx[0]) * 0.5f, (mn[1] + mx[1]) * 0.5f, mn[2] };
    bool tpose = (mx[1] - mn[1]) > 0.65f * H;
    // the joints (the Mannequin's at rest; arms out to the sides for a T-pose) and each bone's segment
    uint32_t nb = man->nbones;
    float J[ANIM_MAX_BONES][3], E[ANIM_MAX_BONES][3];
    for (uint32_t b = 0; b < nb; b++)
        for (int k = 0; k < 3; k++) J[b][k] = man->bind_world[b][k][3];
    static const char* const arm[2][4] = { { "upperarm_l", "lowerarm_l", "hand_l", "l" }, { "upperarm_r", "lowerarm_r", "hand_r", "r" } };
    int arm_b[2][3];
    for (int sd = 0; sd < 2; sd++)
        for (int k = 0; k < 3; k++) arm_b[sd][k] = ar_bone(man, arm[sd][k]);
    if (tpose)
        for (int sd = 0; sd < 2; sd++) {
            if (arm_b[sd][0] < 0 || arm_b[sd][1] < 0 || arm_b[sd][2] < 0) continue;
            float* sh = J[arm_b[sd][0]], side = sd ? 1.0f : -1.0f;
            float l1 = fabsf(man->bones[arm_b[sd][1]].bind.t[2]), l2 = fabsf(man->bones[arm_b[sd][2]].bind.t[2]);
            J[arm_b[sd][1]][0] = sh[0], J[arm_b[sd][1]][1] = sh[1] + side * l1, J[arm_b[sd][1]][2] = sh[2];
            J[arm_b[sd][2]][0] = sh[0], J[arm_b[sd][2]][1] = sh[1] + side * (l1 + l2), J[arm_b[sd][2]][2] = sh[2];
        }
    for (uint32_t b = 0; b < nb; b++) { // the segment's end: its (first) child's joint, else a short piece onward
        int child = -1;
        for (uint32_t k = b + 1; k < nb && child < 0; k++)
            if (man->bones[k].parent == (int16_t)b) child = (int)k;
        const char* n = man->bones[b].name;
        if (child >= 0 && strncmp(n, "clavicle", 8)) memcpy(E[b], J[child], 12);
        else {
            float d[3] = { 0, 0, 0 };
            if (strstr(n, "foot")) d[0] = 18.0f, d[2] = -6.0f;
            else if (strstr(n, "hand")) {
                if (tpose) d[1] = strstr(n, "_l") ? -16.0f : 16.0f;
                else d[2] = -16.0f;
            } else if (!strcmp(n, "head")) d[2] = 22.0f;
            for (int k = 0; k < 3; k++) E[b][k] = J[b][k] + d[k];
        }
    }
    // the scene: the Mannequin's skeleton, each triangle on its nearest bone (the normalized mesh)
    SkScene* sc = calloc(1, sizeof *sc);
    if (!sc) return false;
    snprintf(sc->base, sizeof sc->base, "%s", ((const CName*)ecs_get(w, actor, COMP_NAME)) ? ((const CName*)ecs_get(w, actor, COMP_NAME))->text : "AutoRig");
    sc->nb = (int)nb;
    for (uint32_t b = 0; b < nb; b++) {
        sc->bones[b] = man->bones[b];
        sc->rest[b] = m4_id();
        for (int k = 0; k < 3; k++) sc->rest[b].m[12 + k] = man->bind_world[b][k][3];
    }
    sc->tris = malloc(sizeof(SkTri) * ntri); // (a tool's run, not a frame)
    if (!sc->tris) return free(sc), false;
    uint32_t counts[ANIM_MAX_BONES] = { 0 };
    for (size_t t = 0; t < ntri; t++) {
        SkTri* st = &sc->tris[sc->ntris++];
        float q[3] = { 0, 0, 0 };
        for (int k = 0; k < 3; k++) {
            const GpuVertex* gv = &v[(int32_t)idx[fi + t * 3 + (size_t)k] + vo];
            for (int a = 0; a < 3; a++) st->p[k][a] = (gv->pos[a] * at->scale[a] - c[a]) * s, q[a] += st->p[k][a] / 3.0f;
            // the normal (octahedral) back to a vector
            float ox = (float)(int16_t)(gv->normal_oct & 0xFFFF) / 32767.0f, oy = (float)(int16_t)(gv->normal_oct >> 16) / 32767.0f;
            float nz = 1.0f - fabsf(ox) - fabsf(oy), nx = ox, ny = oy;
            if (nz < 0.0f) nx = (1.0f - fabsf(oy)) * (ox >= 0.0f ? 1.0f : -1.0f), ny = (1.0f - fabsf(ox)) * (oy >= 0.0f ? 1.0f : -1.0f);
            float nl = sqrtf(nx * nx + ny * ny + nz * nz);
            st->n[k][0] = nx / nl, st->n[k][1] = ny / nl, st->n[k][2] = nz / nl;
            st->uv[k][0] = gv->uv[0], st->uv[k][1] = 1.0f - gv->uv[1];
        }
        float best = 1e30f;
        st->bone = pelvis;
        for (uint32_t b = 1; b < nb; b++) { // (not the root: it sits on the floor)
            float d = ar_seg_dist(q, J[b], E[b]);
            if (d < best) best = d, st->bone = (int)b;
        }
        st->mat = sm->material ? 0 : -1;
        counts[st->bone]++;
    }
    // a T-pose's arms laid down: about the shoulder, out to the side -> down (about X: +90 left, -90 right)
    if (tpose)
        for (size_t t = 0; t < sc->ntris; t++) {
            SkTri* st = &sc->tris[t];
            for (int sd = 0; sd < 2; sd++) {
                bool on = false;
                for (int k = 0; k < 3; k++) on |= st->bone == arm_b[sd][k];
                if (!on) continue;
                const float* sh = J[arm_b[sd][0]];
                float ang = (sd ? -90.0f : 90.0f) * 0.0174533f, cs = cosf(ang), sn = sinf(ang);
                for (int k = 0; k < 3; k++) {
                    float y = st->p[k][1] - sh[1], z = st->p[k][2] - sh[2];
                    st->p[k][1] = sh[1] + y * cs - z * sn, st->p[k][2] = sh[2] + y * sn + z * cs;
                    float ny = st->n[k][1], nz = st->n[k][2];
                    st->n[k][1] = ny * cs - nz * sn, st->n[k][2] = ny * sn + nz * cs;
                }
            }
        }
    char disk[1100], sk_name[160];
    snprintf(disk, sizeof disk, "%s%s", content_root, !strncmp(folder_vpath, "/Content", 8) ? folder_vpath + 8 : folder_vpath);
    re_make_dirs(disk);
    uint32_t mh = sm->material;
    g_import_src[0] = 0, g_import_rigid = true; // (parts by bone on purpose)
    bool ok = sk_write_mesh(sc, &mh, sm->material ? 1 : 0, disk, folder_vpath, sk_name, sizeof sk_name, r);
    g_import_rigid = false;
    uint32_t used = 0;
    for (uint32_t b = 0; b < nb; b++) used += counts[b] != 0;
    if (ok) { // the actor: where the statue stood, its size given back by the scale, the idle playing
        CTransform keep = *at;
        const AnimSeq* idle = anim_seq_name("MM_Idle");
        sk_actor(w, sc->base, sk_name, idle ? idle->hash : 0, folder_vpath, r);
        Entity e = ENTITY_NONE; // (the newest Skeletal Mesh actor: sk_actor just made it)
        for (uint32_t i = w->slot_count; i-- > 1 && e == ENTITY_NONE;) {
            Entity x = ecs_entity_at(w, i);
            if (x && ecs_get(w, x, COMP_SKELETAL_MESH) && !ecs_get(w, x, COMP_ATTACH)) e = x;
        }
        CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (t) {
            t->location[0] = keep.location[0] + c[0], t->location[1] = keep.location[1] + c[1], t->location[2] = keep.location[2] + c[2];
            t->rotation[1] = keep.rotation[1];
            t->scale[0] = t->scale[1] = t->scale[2] = 1.0f / s;
        }
        CTransform* ot = ecs_get(w, actor, COMP_TRANSFORM);
        if (ot) ot->flags |= TRANSFORM_HIDDEN | TRANSFORM_HIDDEN_GAME, ecs_touch(w, actor); // (the statue: hidden, kept)
    }
    snprintf(r->text, sizeof r->text, "%s: %u bones with triangles, %s, scale %.2f%s", ok ? sk_name : "failed", used, tpose ? "T-pose (arms laid down)" : "arms down",
             1.0f / s, ok ? "" : " (nothing written)");
    free(sc->tris), free(sc);
    return ok;
}

// The tests' humanoid statue: a Mannequin's parts at rest as one Static Mesh (arms out when tpose), into disk (.rmesh)
bool autorig_test_statue(const char* disk, const char* body, bool tpose) {
    const SkelMesh* m = anim_mesh_name(body);
    if (!m) return false;
    TriOut* out = malloc(sizeof(TriOut) * 200000);
    if (!out) return false;
    uint32_t n = 0;
    int ua[2] = { ar_bone(m, "upperarm_l"), ar_bone(m, "upperarm_r") };
    for (uint32_t p = 0; p < m->nparts && n < 190000; p++) {
        const SkPart* pt = &m->parts[p];
        float rows[3][4];
        anim_part_rows(pt, m->bind_world[pt->bone], rows);
        // (an arm's part, for a T-pose: turned out about its shoulder)
        int side = -1;
        for (int b = pt->bone; b >= 0 && side < 0; b = m->bones[b].parent)
            for (int sd = 0; sd < 2; sd++)
                if (b == ua[sd]) side = sd;
        const GpuVertex* v;
        const uint32_t* idx;
        uint32_t fi, ic;
        int32_t vo;
        render_mesh_cpu(pt->mesh, &v, &idx, &fi, &ic, &vo);
        for (uint32_t t = 0; t + 2 < ic && n < 190000; t += 3) {
            TriOut* o = &out[n++];
            for (int k = 0; k < 3; k++) {
                const GpuVertex* g = &v[(int32_t)idx[fi + t + (uint32_t)k] + vo];
                float q[3];
                for (int a = 0; a < 3; a++) q[a] = rows[a][0] * g->pos[0] + rows[a][1] * g->pos[1] + rows[a][2] * g->pos[2] + rows[a][3];
                if (tpose && side >= 0) {
                    const float* sh = &m->bind_world[ua[side]][0][3];
                    float shp[3] = { m->bind_world[ua[side]][0][3], m->bind_world[ua[side]][1][3], m->bind_world[ua[side]][2][3] };
                    (void)sh;
                    float ang = (side ? 90.0f : -90.0f) * 0.0174533f, cs = cosf(ang), sn = sinf(ang); // (down -> out)
                    float y = q[1] - shp[1], z = q[2] - shp[2];
                    q[1] = shp[1] + y * cs - z * sn, q[2] = shp[2] + y * sn + z * cs;
                }
                memcpy(o->p[k], q, 12);
                o->uv[k][0] = g->uv[0], o->uv[k][1] = g->uv[1];
            }
            float e1[3], e2[3], nn[3];
            for (int a = 0; a < 3; a++) e1[a] = o->p[1][a] - o->p[0][a], e2[a] = o->p[2][a] - o->p[0][a];
            nn[0] = e1[1] * e2[2] - e1[2] * e2[1], nn[1] = e1[2] * e2[0] - e1[0] * e2[2], nn[2] = e1[0] * e2[1] - e1[1] * e2[0];
            float l = sqrtf(nn[0] * nn[0] + nn[1] * nn[1] + nn[2] * nn[2]) + 1e-12f;
            for (int k = 0; k < 3; k++)
                for (int a = 0; a < 3; a++) o->n[k][a] = nn[a] / l;
        }
    }
    bool ok = write_rmesh(disk, out, n, false);
    free(out);
    return ok;
}
