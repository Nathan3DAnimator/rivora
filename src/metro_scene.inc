// The abandoned metro (--scene metro; notas-dev/demos.md "Metrô abandonado"): an original real-time scene after a
// reference picture the user gave (a long underground rail hall, its roof broken open, the sun's shafts in the dust, an
// old train car on the right, wet concrete, puddles, weeds between the rails). Built only from what the engine has:
//   - CC0 assets of the demo pipeline (data/demo/manifest.tsv, the "Metro" rows: ambientCG concrete, rust, painted metal,
//     gravel, corrugated steel, the graffiti atlas and a leak decal; Poly Haven weeds, ferns, shrubs and junk), read from
//     the build's DemoContent at 1K and made the project's (kit_material / kit_prop);
//   - the hall of World Aligned boxes and I-beams (a profile mesh), the rails a Spline Mesh with sleepers as Spline
//     Instances along the curve, the gravel bed another Spline Mesh;
//   - the train car built of parts (body panels, recessed windows, doors, roof, bogies and wheels, the cab), graffiti and
//     rust streaks as Decals;
//   - weeds and grass (a tuft mesh made here) as a Foliage actor painted where the sun falls and along the tracks;
//   - the floor a Wet Surface and puddle Decals (a smooth film: the ray traced / screen space reflections);
//   - the sun through the roof's gaps in a volumetric fog (the shafts: weather.comp reads the sun's virtual shadow maps
//     in each froxel), the sky light, RealGI, a dust particle system, a warm light past the far tunnel's mouth.
// Without DemoContent (an offline build) the materials answer 0 and the scene keeps its plain colors.
// The level is saved into the project and made its startup map (editor and game). Included by editor.c after
// editor_spline.inc (its profile meshes and spline actors) and haunted_house.inc (hh_box).

static uint32_t g_mt_floor, g_mt_wall, g_mt_steel, g_mt_rail, g_mt_train, g_mt_train_low, g_mt_dark, g_mt_gravel, g_mt_sleeper, g_mt_roof,
    g_mt_glass, g_mt_black, g_mt_puddle, g_mt_grass, g_mt_leak, g_mt_graffiti[6];
static uint32_t g_mt_ibeam, g_mt_track, g_mt_bed, g_mt_tuft; // (the meshes made here)
static uint32_t g_mt_carroof, g_mt_redpaint; // (2026-11-20: the cars' rounded roof, their red stripe)
static uint32_t g_mt_ivy; // (2026-11-20: ivy hanging from ledges)

// The layout below is written with +y to the camera's left (as the reference is read); the engine's +Y is to the right
// (Unreal's axes): every y, yaw and roll goes through MT_Y, a mirror across the hall's axis (a box stays a box)
#define MT_Y(v) (-(v))

// A box of size (sx, sy, sz) cm centered at (x, y, z), turned (pitch, yaw, roll degrees), of a material
static Entity mt_obb(Editor* ed, float x, float y, float z, float sx, float sy, float sz, float pitch, float yaw, float roll, uint32_t mat,
                     const float col[3]) {
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    Entity e = mesh_actor(ed, SHAPE_CUBE, x, MT_Y(y), z, col ? col : grey, 0, 0.8f);
    CTransform* t = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
    if (!t) return e;
    t->scale[0] = sx / 100.0f, t->scale[1] = sy / 100.0f, t->scale[2] = sz / 100.0f;
    t->rotation[0] = pitch, t->rotation[1] = MT_Y(yaw), t->rotation[2] = MT_Y(roll), t->flags = MOBILITY_STATIC;
    if (mat) ((CStaticMesh*)ecs_get(&ed->world, e, COMP_STATIC_MESH))->material = mat;
    return e;
}
// An actor of a mesh made here (or a shape), placed, turned and scaled
static Entity mt_mesh(Editor* ed, uint32_t mesh, float x, float y, float z, float pitch, float yaw, float roll, float sx, float sy, float sz,
                      uint32_t mat, const float col[3]) {
    Entity e = mt_obb(ed, x, y, z, sx * 100.0f, sy * 100.0f, sz * 100.0f, pitch, yaw, roll, mat, col);
    CStaticMesh* sm = e ? ecs_get(&ed->world, e, COMP_STATIC_MESH) : NULL;
    if (sm && (mesh < SHAPE_COUNT || render_mesh_slot(mesh) >= SHAPE_COUNT)) sm->mesh = mesh; // (a shape, or a mesh that loaded)
    return e;
}

// A box from (x0, y0, z0) to (x1, y1, z1) cm (haunted_house.inc's, World Aligned), a Decal (Life Span 0), the Foliage
// brush: in the layout's mirrored y
static Entity mt_box(Editor* ed, float x0, float y0, float z0, float x1, float y1, float z1, uint32_t mat, const float col[3]) {
    return hh_box(ed, x0, MT_Y(y1), z0, x1, MT_Y(y0), z1, mat, col);
}
static Entity mt_decal(World* w, uint32_t mat, float x, float y, float z, float pitch, float yaw, float roll, const float size[3]) {
    return mat ? decal_spawn(w, mat, (float[3]){ x, MT_Y(y), z }, (float[3]){ pitch, MT_Y(yaw), MT_Y(roll) }, size, 0.0f) : ENTITY_NONE;
}
static void mt_paint(World* w, Entity fol, float x, float y, float z, float radius, float density) {
    foliage_paint(w, fol, (float[3]){ x, MT_Y(y), z }, radius, density, false, 1.0f);
}

// A mesh along X of one or more profiles (loops of points y z, each clockwise seen down +X: its faces out), n_len rings
// over len cm, written as /Content/Meshes/<name>.rmesh when missing (the spline profile mesh, several loops: two rails)
static uint32_t mt_profile_mesh(Editor* ed, const char* name, const float (*prof)[2], const int* loop_n, int nloops, bool closed, float len, int n_len) {
    char vp[200], path[1100];
    snprintf(vp, sizeof vp, "/Content/Meshes/%s.rmesh", name);
    uint32_t id = render_mesh_id_of(vp);
    if (id && render_mesh_slot(id) >= SHAPE_COUNT) return id;
    snprintf(path, sizeof path, "%s/Meshes", content_root());
    re_make_dirs(path);
    snprintf(path, sizeof path, "%s/Meshes/%s.rmesh", content_root(), name);
    int np_all = 0;
    for (int l = 0; l < nloops; l++) np_all += loop_n[l];
    uint32_t nv = (uint32_t)(np_all * 2) * (uint32_t)(n_len + 1), ni = (uint32_t)(np_all * 2) * (uint32_t)n_len * 6u;
    GpuVertex* v = calloc(nv, sizeof(GpuVertex));
    uint32_t* idx = malloc(sizeof(uint32_t) * ni);
    if (v && idx) {
        uint32_t at = 0, ii = 0;
        const float(*p)[2] = prof;
        for (int l = 0; l < nloops; l++) {
            int np = loop_n[l];
            uint32_t edges = (uint32_t)(closed ? np : np - 1);
            float u_acc = 0.0f;
            for (uint32_t e = 0; e < edges; e++) { // (each edge its own two columns: the profile's corners stay sharp)
                const float* a = p[e];
                const float* b = p[(e + 1) % (uint32_t)np];
                float dy = b[0] - a[0], dz = b[1] - a[1], el = sqrtf(dy * dy + dz * dz);
                float n[3] = { 0.0f, -dz / fmaxf(el, 1e-6f), dy / fmaxf(el, 1e-6f) };
                uint32_t base = at;
                for (int r = 0; r <= n_len; r++)
                    for (int k = 0; k < 2; k++) {
                        const float* q = k ? b : a;
                        GpuVertex* g = &v[at++];
                        g->pos[0] = len * (float)r / (float)n_len, g->pos[1] = q[0], g->pos[2] = q[1];
                        g->normal_oct = shapes_oct(n);
                        g->uv[0] = (u_acc + (k ? el : 0.0f)) / 100.0f, g->uv[1] = len * (float)r / (float)n_len / 100.0f;
                        g->uv2[0] = g->uv2[1] = -1.0f;
                    }
                u_acc += el;
                for (int r = 0; r < n_len; r++) {
                    uint32_t q = base + (uint32_t)r * 2u;
                    idx[ii++] = q, idx[ii++] = q + 2, idx[ii++] = q + 1, idx[ii++] = q + 1, idx[ii++] = q + 2, idx[ii++] = q + 3;
                }
            }
            p += np;
        }
        MeshBuilt mb;
        if (mesh_build_indexed(v, at, idx, ii, false, false, &mb)) {
            if (!rmesh_write(path, &mb)) printf("[RealEngine] FAIL: metro: %s not written\n", name);
            mesh_built_free(&mb);
        }
    }
    free(v), free(idx);
    render_meshes_reload(ed->vk);
    return render_mesh_id_of(vp);
}

// A tuft of grass: blades from a small disk, bent outward, narrowing to their tips (uv.y: 0 at the root, 1 at the tip,
// the material's root-to-tip color); its normals lean up (lit like a lawn, not like flat cards). About 60 x 45 cm.
static uint32_t mt_tuft_mesh(Editor* ed) {
    const char* name = "SM_MetroGrassTuft";
    char vp[200], path[1100];
    snprintf(vp, sizeof vp, "/Content/Meshes/%s.rmesh", name);
    uint32_t id = render_mesh_id_of(vp);
    if (id && render_mesh_slot(id) >= SHAPE_COUNT) return id;
    snprintf(path, sizeof path, "%s/Meshes", content_root());
    re_make_dirs(path);
    snprintf(path, sizeof path, "%s/Meshes/%s.rmesh", content_root(), name);
    enum { BLADES = 22, SEGS = 4 };
    GpuVertex v[BLADES * (SEGS + 1) * 2];
    uint32_t idx[BLADES * SEGS * 6], nv = 0, ni = 0, rng = 12345u;
#define MT_RND() ((rng = rng * 1664525u + 1013904223u), (float)(rng >> 8) / 16777216.0f)
    for (int b = 0; b < BLADES; b++) {
        float a = MT_RND() * 6.2831853f, r0 = MT_RND() * 9.0f, h = 22.0f + MT_RND() * 26.0f, w = 1.0f + MT_RND() * 0.9f;
        float lean = 0.25f + MT_RND() * 0.55f, face = a + 1.5707963f + (MT_RND() - 0.5f) * 1.2f;
        float ox = cosf(a) * r0, oy = sinf(a) * r0, dx = cosf(a), dy = sinf(a), fx = cosf(face), fy = sinf(face);
        float n[3] = { -fy * 0.6f, fx * 0.6f, 0.8f }; // (the blade's face, leaning up)
        if (n[0] * dx + n[1] * dy < 0.0f) n[0] = -n[0], n[1] = -n[1];
        uint32_t base = nv;
        for (int s = 0; s <= SEGS; s++) {
            float t = (float)s / (float)SEGS, out = lean * h * t * t, half = w * (1.0f - t * 0.92f);
            float cx = ox + dx * out, cy = oy + dy * out, cz = h * (t - 0.18f * lean * t * t);
            for (int k = 0; k < 2; k++) {
                GpuVertex* g = &v[nv++];
                float sgn = k ? 1.0f : -1.0f;
                g->pos[0] = cx + fx * half * sgn, g->pos[1] = cy + fy * half * sgn, g->pos[2] = cz;
                g->normal_oct = shapes_oct(n);
                g->uv[0] = (float)k, g->uv[1] = t;
                g->uv2[0] = g->uv2[1] = -1.0f;
            }
        }
        for (int s = 0; s < SEGS; s++) {
            uint32_t q = base + (uint32_t)s * 2u;
            idx[ni++] = q, idx[ni++] = q + 1, idx[ni++] = q + 2, idx[ni++] = q + 1, idx[ni++] = q + 3, idx[ni++] = q + 2;
        }
    }
#undef MT_RND
    MeshBuilt mb;
    if (mesh_build_indexed(v, nv, idx, ni, false, false, &mb)) {
        if (!rmesh_write(path, &mb)) printf("[RealEngine] FAIL: metro: %s not written\n", name);
        mesh_built_free(&mb);
    }
    render_meshes_reload(ed->vk);
    return render_mesh_id_of(vp);
}

// (2026-11-20) Ivy hanging from a ledge: 14 strands from a 1.2 m line at z 0 down 1.5-4 m, wavering, each with a leaf
// every 9-14 cm (a quad 7-12 cm, turned at random but facing out of the wall, +X); uv.y 0.55-1 (the grass material's
// green, not its root). Two-sided by its material
static uint32_t mt_ivy_mesh(Editor* ed) {
    const char* name = "SM_MetroIvy";
    char vp[200], path[1100];
    snprintf(vp, sizeof vp, "/Content/Meshes/%s.rmesh", name);
    uint32_t id = render_mesh_id_of(vp);
    if (id && render_mesh_slot(id) >= SHAPE_COUNT) return id;
    snprintf(path, sizeof path, "%s/Meshes", content_root());
    re_make_dirs(path);
    snprintf(path, sizeof path, "%s/Meshes/%s.rmesh", content_root(), name);
    enum { STRANDS = 14, MAXL = 44 };
    static GpuVertex v[STRANDS * MAXL * 4];
    static uint32_t idx[STRANDS * MAXL * 6];
    uint32_t nv = 0, ni = 0, rng = 777u;
#define MT_RND() ((rng = rng * 1664525u + 1013904223u), (float)(rng >> 8) / 16777216.0f)
    for (int st = 0; st < STRANDS; st++) {
        float y0 = -60.0f + 120.0f * MT_RND(), len = 150.0f + 250.0f * MT_RND(), ph = MT_RND() * 6.28f, amp = 4.0f + 10.0f * MT_RND();
        for (float d = 0.0f; d < len && nv + 4 <= STRANDS * MAXL * 4; d += 9.0f + 5.0f * MT_RND()) {
            float t = d / len, cy = y0 + sinf(ph + d * 0.035f) * amp + (MT_RND() - 0.5f) * 6.0f, cz = -d, cx = 3.0f + MT_RND() * 8.0f;
            float sz = (7.0f + 5.0f * MT_RND()) * (1.0f - 0.35f * t), a = (MT_RND() - 0.5f) * 2.2f, tilt = (MT_RND() - 0.5f) * 1.2f;
            float ux = sinf(a) * sz * 0.5f, uy = cosf(a) * sz * 0.5f, vz = sz * 0.5f;
            float n[3] = { cosf(a) * cosf(tilt), -sinf(a), sinf(tilt) };
            if (n[0] < 0.2f) n[0] = 0.2f;
            const float q[4][3] = { { cx - ux, cy - uy, cz - vz }, { cx + ux, cy + uy, cz - vz }, { cx + ux, cy + uy, cz + vz }, { cx - ux, cy - uy, cz + vz } };
            for (int k = 0; k < 4; k++) {
                GpuVertex* g = &v[nv + (uint32_t)k];
                memset(g, 0, sizeof *g);
                memcpy(g->pos, q[k], 12);
                g->normal_oct = shapes_oct(n);
                g->uv[0] = (k == 1 || k == 2) ? 1.0f : 0.0f, g->uv[1] = 0.55f + 0.45f * (k >= 2 ? 1.0f : 0.6f);
                g->uv2[0] = g->uv2[1] = -1.0f;
            }
            idx[ni++] = nv, idx[ni++] = nv + 1, idx[ni++] = nv + 2, idx[ni++] = nv, idx[ni++] = nv + 2, idx[ni++] = nv + 3;
            nv += 4;
        }
    }
#undef MT_RND
    MeshBuilt mb;
    if (mesh_build_indexed(v, nv, idx, ni, false, false, &mb)) {
        if (!rmesh_write(path, &mb)) printf("[RealEngine] FAIL: metro: %s not written\n", name);
        mesh_built_free(&mb);
    }
    render_meshes_reload(ed->vk);
    return render_mesh_id_of(vp);
}

// (2026-11-20) A heap of rubble: n broken pieces of concrete (slabs and chunks, 12-90 cm) tumbled around (cx, cy) within
// radius r, higher toward its middle; a few loose planks on it
static void mt_rubble(Editor* ed, float cx, float cy, float z0, float r, int n, uint32_t seed) {
    const float concrete[3] = { 0.32f, 0.31f, 0.3f }, wood[3] = { 0.18f, 0.13f, 0.09f };
    uint32_t rr = seed * 2654435761u + 7u;
#define MT_R(a, b) ((rr = rr * 1664525u + 1013904223u), (a) + ((b) - (a)) * (float)(rr >> 8) / 16777216.0f)
    for (int k = 0; k < n; k++) {
        float a = MT_R(0.0f, 6.2832f), d = r * sqrtf(MT_R(0.0f, 1.0f)), h = (1.0f - d / r) * r * 0.45f;
        float x = cx + cosf(a) * d, y = cy + sinf(a) * d * 0.8f;
        bool slab = MT_R(0.0f, 1.0f) < 0.35f;
        float sx = slab ? MT_R(50.0f, 90.0f) : MT_R(12.0f, 45.0f), sy = slab ? MT_R(35.0f, 70.0f) : sx * MT_R(0.6f, 1.1f), sz = slab ? MT_R(6.0f, 12.0f) : sx * MT_R(0.5f, 0.9f);
        mt_obb(ed, x, y, z0 + h * MT_R(0.4f, 1.0f) + sz * 0.3f, sx, sy, sz, MT_R(-35.0f, 35.0f), MT_R(0.0f, 180.0f), MT_R(-30.0f, 30.0f), g_mt_wall, concrete);
    }
    for (int k = 0; k < n / 8; k++) // (a loose sleeper or plank across it)
        mt_obb(ed, cx + MT_R(-r, r) * 0.6f, cy + MT_R(-r, r) * 0.5f, z0 + r * 0.25f, MT_R(120.0f, 220.0f), 18.0f, 10.0f, MT_R(-15.0f, 15.0f), MT_R(0.0f, 180.0f),
               MT_R(-20.0f, 20.0f), g_mt_sleeper, wood);
#undef MT_R
}

// --- Materials of the scene's own (procedural or of the decal textures), under Content/Demo/Metro/Materials ---
static void mt_set(MatGraph* g, int node, const char* sock, float a, float b, float c, float d) {
    int k = kit_sock(g, node, false, sock);
    if (k < 0) return;
    g->nodes[node].in[k][0] = a, g->nodes[node].in[k][1] = b, g->nodes[node].in[k][2] = c, g->nodes[node].in[k][3] = d;
}
static int mt_ramp(MatGraph* g, float x, float y, const float s0[5], const float s1[5]) {
    int r = kit_node(g, NODE_COLOR_RAMP, x, y);
    g->nodes[r].ramp_count = 2;
    memcpy(g->nodes[r].ramp[0], s0, sizeof(float) * 5), memcpy(g->nodes[r].ramp[1], s1, sizeof(float) * 5);
    return r;
}
static int mt_image(MatGraph* g, float x, float y, const char* id, const char* map, bool color) {
    int n = kit_node(g, NODE_TEX_IMAGE, x, y);
    snprintf(g->nodes[n].image, MAT_PATH, "/Content/Demo/Metro/Textures/%s/%s_%s.jpg", id, id, map);
    if (!color) g->nodes[n].prop[2] = IMAGE_COLORSPACE_NONCOLOR;
    return n;
}
// A decal texture set of the build's DemoContent copied into the project (its maps: Color, Opacity, NormalGL, Roughness)
static bool mt_copy_maps(const char* id) {
    char src[800], tdir[900];
    if (!kit_dir(src, sizeof src)) return false;
    snprintf(tdir, sizeof tdir, "%s/Demo/Metro/Textures/%s", content_root(), id);
    re_make_dirs(tdir);
    static const char* const maps[4] = { "Color", "Opacity", "NormalGL", "Roughness" };
    bool ok = true;
    for (int k = 0; k < 4; k++) {
        char from[1000], to[1000];
        snprintf(from, sizeof from, "%s/Textures/%s/%s_%s-JPG_%s.jpg", src, id, id, g_kit_tres, maps[k]);
        snprintf(to, sizeof to, "%s/%s_%s.jpg", tdir, id, maps[k]);
        bool have = re_file_stat(to, NULL, NULL, NULL) || (re_file_stat(from, NULL, NULL, NULL) && re_copy_file(from, to));
        if (k < 2) ok &= have;
    }
    return ok;
}

typedef void (*MtBuild)(MatGraph* g, int bsdf, int out, int arg);
static uint32_t mt_material(const char* name, MtBuild build, int arg, int res, float world_tile, bool two_sided) {
    char dir[900], path[1000];
    snprintf(dir, sizeof dir, "%s/Demo/Metro/Materials", content_root());
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s/M_%s.rmat", dir, name);
    Material* m = re_file_stat(path, NULL, NULL, NULL) ? mat_load(path) : mat_create(path, false, NULL);
    if (!m || !m->graph) return 0;
    MatGraph* g = m->graph;
    mat_graph_default(g);
    int bsdf = -1, out = -1;
    for (int i = 0; i < g->node_count; i++) {
        if (g->nodes[i].type == NODE_BSDF_PRINCIPLED) bsdf = i;
        if (g->nodes[i].type == NODE_OUTPUT_MATERIAL) out = i;
    }
    build(g, bsdf, out, arg);
    m->resolution = res;
    m->world_aligned = world_tile > 0.0f;
    m->world_tile = world_tile > 0.0f ? world_tile : 200.0f;
    m->two_sided = two_sided;
    mat_save(m);
    mat_mark_dirty(m);
    return m->hash;
}
// A kit material made wet (the Wet Surface node between its Principled and the output): mask MAT_WET_MASK_
static void mt_wet(uint32_t hash, int mask, float wetness, float film, float droplets) {
    Material* m = hash ? mat_find_hash(hash) : NULL;
    if (!m || !m->graph) return;
    MatGraph* g = m->graph;
    int bsdf = -1, out = -1;
    for (int i = 0; i < g->node_count; i++) {
        if (g->nodes[i].type == NODE_BSDF_PRINCIPLED) bsdf = i;
        if (g->nodes[i].type == NODE_OUTPUT_MATERIAL) out = i;
        if (g->nodes[i].type == NODE_RE_WET) return; // (already)
    }
    if (bsdf < 0 || out < 0) return;
    int w = kit_node(g, NODE_RE_WET, 120, 0);
    g->nodes[w].prop[0] = mask;
    kit_link(g, bsdf, "BSDF", w, "Shader");
    kit_link(g, w, "Shader", out, "Surface");
    mt_set(g, w, "Wetness", wetness, 0, 0, 0);
    mt_set(g, w, "Water Film", film, 0, 0, 0);
    mt_set(g, w, "Droplets", droplets, 0, 0, 0);
    mt_set(g, w, "Running", 0.0f, 0, 0, 0);
    mat_save(m);
    mat_mark_dirty(m);
}

// The train's windows: dark glass under years of dust (grime in streaks and blotches, rough where it is dirty)
static void mt_glass_build(MatGraph* g, int bsdf, int out, int arg) {
    (void)out, (void)arg;
    int nz = kit_node(g, NODE_TEX_NOISE, -900, 0);
    mt_set(g, nz, "Scale", 9.0f, 0, 0, 0), mt_set(g, nz, "Detail", 8.0f, 0, 0, 0);
    const float c0[5] = { 0.35f, 0.005f, 0.006f, 0.007f, 1 }, c1[5] = { 0.75f, 0.026f, 0.022f, 0.017f, 1 };
    int rc = mt_ramp(g, -600, 0, c0, c1);
    kit_link(g, nz, "Fac", rc, "Fac");
    kit_link(g, rc, "Color", bsdf, "Base Color");
    const float r0[5] = { 0.35f, 0.22f, 0.22f, 0.22f, 1 }, r1[5] = { 0.75f, 0.6f, 0.6f, 0.6f, 1 };
    int rr = mt_ramp(g, -600, 250, r0, r1);
    kit_link(g, nz, "Fac", rr, "Fac");
    kit_link(g, rr, "Color", bsdf, "Roughness");
    mt_set(g, bsdf, "Metallic", 0.0f, 0, 0, 0);
}
// The black of the tunnels beyond the hall (no light comes back from them)
static void mt_black_build(MatGraph* g, int bsdf, int out, int arg) {
    (void)out, (void)arg;
    mt_set(g, bsdf, "Base Color", 0.004f, 0.004f, 0.004f, 1);
    mt_set(g, bsdf, "Roughness", 1.0f, 0, 0, 0);
}
// (2026-11-20) The cars' stripe: old red paint, faded and dirty in blotches (noise), half matte
static void mt_redpaint_build(MatGraph* g, int bsdf, int out, int arg) {
    (void)out, (void)arg;
    int noise = kit_node(g, NODE_TEX_NOISE, -900, 0);
    mt_set(g, noise, "Scale", 9.0f, 0, 0, 0), mt_set(g, noise, "Detail", 6.0f, 0, 0, 0);
    const float a0[5] = { 0.35f, 0.09f, 0.03f, 0.025f, 1 }, a1[5] = { 0.7f, 0.42f, 0.08f, 0.05f, 1 };
    int ra = mt_ramp(g, -500, 0, a0, a1);
    kit_link(g, noise, "Fac", ra, "Fac");
    kit_link(g, ra, "Color", bsdf, "Base Color");
    mt_set(g, bsdf, "Roughness", 0.62f, 0, 0, 0);
    mt_set(g, bsdf, "Metallic", 0.15f, 0, 0, 0);
}
// A puddle (a Decal): a smooth dark film, its edge broken by noise (an ellipse of the decal's box)
static void mt_puddle_build(MatGraph* g, int bsdf, int out, int arg) {
    (void)out, (void)arg;
    int tc = kit_node(g, NODE_TEX_COORD, -1200, 0), sub = kit_node(g, NODE_VECTOR_MATH, -1000, 0), grad = kit_node(g, NODE_TEX_GRADIENT, -800, 0);
    int noise = kit_node(g, NODE_TEX_NOISE, -1000, 260), madd = kit_node(g, NODE_MATH, -800, 260), mul = kit_node(g, NODE_MATH, -600, 100);
    kit_link(g, tc, "UV", sub, "Vector");
    g->nodes[sub].prop[0] = VMATH_SUBTRACT;
    mt_set(g, sub, "Vector_001", 0.5f, 0.5f, 0.0f, 0);
    g->nodes[grad].prop[0] = GRADIENT_SPHERICAL;
    kit_link(g, sub, "Vector", grad, "Vector");
    kit_link(g, tc, "UV", noise, "Vector");
    mt_set(g, noise, "Scale", 4.0f, 0, 0, 0), mt_set(g, noise, "Detail", 5.0f, 0, 0, 0);
    g->nodes[madd].prop[0] = MATH_MULTIPLY_ADD; // (noise * 0.9 + 0.55: a ragged shore)
    kit_link(g, noise, "Fac", madd, "Value");
    mt_set(g, madd, "Value_001", 0.9f, 0, 0, 0), mt_set(g, madd, "Value_002", 0.55f, 0, 0, 0);
    g->nodes[mul].prop[0] = MATH_MULTIPLY;
    kit_link(g, grad, "Fac", mul, "Value");
    kit_link(g, madd, "Value", mul, "Value_001");
    const float a0[5] = { 0.42f, 0, 0, 0, 1 }, a1[5] = { 0.5f, 1, 1, 1, 1 };
    int ra = mt_ramp(g, -400, 100, a0, a1);
    kit_link(g, mul, "Value", ra, "Fac");
    kit_link(g, ra, "Color", bsdf, "Alpha");
    mt_set(g, bsdf, "Base Color", 0.018f, 0.017f, 0.016f, 1);
    mt_set(g, bsdf, "Roughness", 0.03f, 0, 0, 0);
}
// A graffiti tag of the ambientCG atlas (4 x 4 tags; arg: which), faded and dirty on the old paint
static void mt_graffiti_build(MatGraph* g, int bsdf, int out, int arg) {
    (void)out;
    int tc = kit_node(g, NODE_TEX_COORD, -1200, 0), map = kit_node(g, NODE_MAPPING, -1000, 0);
    kit_link(g, tc, "UV", map, "Vector");
    mt_set(g, map, "Location", (float)(arg % 4) * 0.25f, (float)(arg / 4) * 0.25f, 0, 0);
    mt_set(g, map, "Scale", 0.25f, 0.25f, 1.0f, 0);
    int col = mt_image(g, -760, 0, "GraffitiSet001", "Color", true), op = mt_image(g, -760, 300, "GraffitiSet001", "Opacity", false);
    kit_link(g, map, "Vector", col, "Vector"), kit_link(g, map, "Vector", op, "Vector");
    int mix = kit_node(g, NODE_MIX, -420, 0);
    g->nodes[mix].prop[1] = BLEND_MUL;
    mt_set(g, mix, "Factor", 1.0f, 0, 0, 0);
    kit_link(g, col, "Color", mix, "A_Color");
    mt_set(g, mix, "B_Color", 0.75f, 0.72f, 0.68f, 1); // (faded under the grime)
    kit_link(g, mix, "Result_Color", bsdf, "Base Color");
    int am = kit_node(g, NODE_MATH, -420, 300);
    g->nodes[am].prop[0] = MATH_MULTIPLY;
    kit_link(g, op, "Color", am, "Value");
    mt_set(g, am, "Value_001", 0.9f, 0, 0, 0);
    kit_link(g, am, "Value", bsdf, "Alpha");
    mt_set(g, bsdf, "Roughness", 0.6f, 0, 0, 0);
}
// Rust and water streaks running down from the windows (ambientCG Leaking 006)
static void mt_leak_build(MatGraph* g, int bsdf, int out, int arg) {
    (void)out, (void)arg;
    int col = mt_image(g, -760, 0, "Leaking006", "Color", true), op = mt_image(g, -760, 300, "Leaking006", "Opacity", false);
    int mix = kit_node(g, NODE_MIX, -420, 0);
    g->nodes[mix].prop[1] = BLEND_MUL;
    mt_set(g, mix, "Factor", 1.0f, 0, 0, 0);
    kit_link(g, col, "Color", mix, "A_Color");
    mt_set(g, mix, "B_Color", 0.5f, 0.32f, 0.2f, 1); // (rust brown)
    kit_link(g, mix, "Result_Color", bsdf, "Base Color");
    kit_link(g, op, "Color", bsdf, "Alpha");
    mt_set(g, bsdf, "Roughness", 0.8f, 0, 0, 0);
}
// The grass: dark at the root, yellow-green to the tips, a little brown here and there (the uv.y of the tuft's blades)
static void mt_grass_build(MatGraph* g, int bsdf, int out, int arg) {
    (void)out, (void)arg;
    int tc = kit_node(g, NODE_TEX_COORD, -1000, 0), sep = kit_node(g, NODE_SEPARATE_XYZ, -800, 0);
    kit_link(g, tc, "UV", sep, "Vector");
    const float c0[5] = { 0.0f, 0.018f, 0.028f, 0.008f, 1 }, c1[5] = { 1.0f, 0.12f, 0.16f, 0.045f, 1 };
    int rc = mt_ramp(g, -600, 0, c0, c1);
    kit_link(g, sep, "Y", rc, "Fac");
    kit_link(g, rc, "Color", bsdf, "Base Color");
    mt_set(g, bsdf, "Roughness", 0.55f, 0, 0, 0);
}

// One prop placed n times (x, y, z, yaw, scale each), casting shadows; its first part's mesh and material (Foliage)
static void mt_props(Editor* ed, const char* id, int n, const float (*at)[5], uint32_t* mesh, uint32_t* mat) {
    KitProp p = kit_prop(ed, "Metro", id);
    if (!p.n) {
        printf("[RealEngine] FAIL: metro: %s missing\n", id);
        return;
    }
    if (mesh && mat) { // (a Foliage type is one mesh: the plant's part with the most of its bounds, its triangles told)
        float best = -1.0f;
        for (uint32_t i = 0; i < p.n; i++) {
            const CStaticMesh* sm = ecs_get(&ed->world, p.parts[i], COMP_STATIC_MESH);
            float mn[3], mx[3];
            uint32_t tris[4] = { 0 };
            float err[4];
            if (!sm) continue;
            render_mesh_bounds(render_mesh_slot(sm->mesh), mn, mx);
            render_mesh_lods(sm->mesh, tris, err);
            float vol = (mx[0] - mn[0]) * (mx[1] - mn[1]) * (mx[2] - mn[2]);
            printf("[RealEngine] Metro: %s part %u: %u triangles, %.0f x %.0f x %.0f cm\n", id, i, tris[0], mx[0] - mn[0], mx[1] - mn[1], mx[2] - mn[2]);
            if (vol > best) best = vol, *mesh = sm->mesh, *mat = sm->material;
        }
    }
    for (int i = 0; i < n; i++) kit_place(ed, &p, at[i][0], MT_Y(at[i][1]), at[i][2], MT_Y(at[i][3]), at[i][4], false);
    kit_done(ed, &p);
}

// --- The train: a car of length len from x0 on the track at y (its side to +y faces the camera) ---
#define MT_TRACK_B (-330.0f)
// track A's center across the hall at x (its curve to the left past x 400, into the tunnel)
static float mt_track_a_y(float x) { return 250.0f + (x > 400.0f ? (x - 400.0f) * 0.06f : 0.0f); }
static void mt_car(Editor* ed, float x0, float len, bool cab, bool cab_near, uint32_t seed) {
    const float y = MT_TRACK_B, hw = 140.0f; // (half its width)
    const float zb = 135.0f, zw0 = 235.0f, zw1 = 335.0f, zt = 395.0f;
    const float dark[3] = { 0.03f, 0.028f, 0.026f }, green[3] = { 0.16f, 0.2f, 0.19f };
    uint32_t tm = g_mt_train, lm = g_mt_train, dm = g_mt_dark; // (2026-11-20: old grey steel all over, as the reference's; the paint a stripe)
    // the doors (two double doors a side, 130 wide), the windows between them (110 wide, piers of 40)
    float doors[2] = { x0 + 330.0f, x0 + len - 460.0f };
    // the glass core behind the window band, the lower panels, the band over the windows, the belt rib
    mt_obb(ed, x0 + len * 0.5f, y, (zw0 + zw1) * 0.5f, len - 4.0f, hw * 2.0f - 26.0f, zw1 - zw0 + 4.0f, 0, 0, 0, g_mt_glass, dark); // (2026-11-20: 13 cm in: the openings' shade reads them)
    float xa = x0;
    for (int d = 0; d <= 2; d++) {
        float xb = d < 2 ? doors[d] : x0 + len;
        if (xb > xa) {
            mt_obb(ed, (xa + xb) * 0.5f, y, (zb + zw0) * 0.5f, xb - xa, hw * 2.0f, zw0 - zb, 0, 0, 0, lm, green);
            // the piers between the windows of this stretch
            int nwin = (int)floorf((xb - xa - 40.0f) / 150.0f);
            float used = (float)nwin * 150.0f + 40.0f, pad = (xb - xa - used) * 0.5f;
            for (int k = 0; k <= nwin; k++) {
                float px0 = xa + pad + (float)k * 150.0f, px1 = px0 + 40.0f;
                if (k == 0) px0 = xa;
                if (k == nwin) px1 = xb;
                mt_obb(ed, (px0 + px1) * 0.5f, y, (zw0 + zw1) * 0.5f, px1 - px0, hw * 2.0f, zw1 - zw0, 0, 0, 0, tm, green);
                if (k < nwin) { // (2026-11-20) the window after this pier: a dark rubber frame and its corners cut at 45 degrees
                    float wx0 = px1, wx1 = xa + pad + (float)(k + 1) * 150.0f, wc = (wx0 + wx1) * 0.5f, ww = wx1 - wx0, wz = (zw0 + zw1) * 0.5f;
                    mt_obb(ed, wc, y, zw0 + 3.0f, ww, hw * 2.0f + 1.0f, 6.0f, 0, 0, 0, dm, dark);
                    mt_obb(ed, wc, y, zw1 - 3.0f, ww, hw * 2.0f + 1.0f, 6.0f, 0, 0, 0, dm, dark);
                    mt_obb(ed, wx0 + 3.0f, y, wz, 6.0f, hw * 2.0f + 1.0f, zw1 - zw0, 0, 0, 0, dm, dark);
                    mt_obb(ed, wx1 - 3.0f, y, wz, 6.0f, hw * 2.0f + 1.0f, zw1 - zw0, 0, 0, 0, dm, dark);
                    for (int c = 0; c < 4; c++)
                        mt_obb(ed, (c & 1) ? wx1 - 4.0f : wx0 + 4.0f, y, (c & 2) ? zw1 - 4.0f : zw0 + 4.0f, 16.0f, hw * 2.0f + 0.5f, 16.0f, 45.0f, 0, 0, tm, green);
                }
            }
        }
        if (d < 2) { // the door: two leaves recessed 3 cm (their lower panels, stiles, the gap), their windows
            float dx = doors[d];
            mt_obb(ed, dx + 65.0f, y, 205.0f, 130.0f, hw * 2.0f - 6.0f, 130.0f, 0, 0, 0, lm, green);
            for (int k = 0; k < 3; k++)
                mt_obb(ed, dx + (k == 0 ? 6.0f : k == 1 ? 65.0f : 124.0f), y, 302.5f, 12.0f, hw * 2.0f - 6.0f, 65.0f, 0, 0, 0, tm, green);
            mt_obb(ed, dx + 65.0f, y, 238.0f, 2.5f, hw * 2.0f - 2.0f, 196.0f, 0, 0, 0, dm, dark);
            mt_obb(ed, dx + 65.0f, y, 302.5f, 126.0f, hw * 2.0f - 16.0f, 65.0f, 0, 0, 0, g_mt_glass, dark);
            xa = dx + 130.0f;
        }
    }
    // (2026-11-20) the upper shell: the rounded roof's mesh (SM_MetroCarRoof, 18 m), its ends closed by plates inside it
    if (g_mt_carroof && len == 1800.0f) {
        mt_mesh(ed, g_mt_carroof, x0, y, 0.0f, 0, 0, 0, 1.0f, 1.0f, 1.0f, tm, green);
        for (int e = 0; e < 2; e++)
            mt_obb(ed, e ? x0 + len - 2.0f : x0 + 2.0f, y, (zw1 + zt + 30.0f) * 0.5f, 4.0f, hw * 2.0f - 4.0f, zt + 30.0f - zw1, 0, 0, 0, tm, green);
    } else
        mt_obb(ed, x0 + len * 0.5f, y, (zw1 + zt) * 0.5f, len, hw * 2.0f, zt - zw1, 0, 0, 0, tm, green);
    mt_obb(ed, x0 + len * 0.5f, y, zw0 - 2.0f, len + 2.0f, hw * 2.0f + 5.0f, 6.0f, 0, 0, 0, tm, green);
    mt_obb(ed, x0 + len * 0.5f, y, zw1 + 3.0f, len + 2.0f, hw * 2.0f + 4.0f, 5.0f, 0, 0, 0, tm, green);
    // the red stripe under the windows (faded paint) and the rain gutters along the eaves
    mt_obb(ed, x0 + len * 0.5f, y, zw0 - 16.0f, len, hw * 2.0f + 3.0f, 18.0f, 0, 0, 0, g_mt_redpaint ? g_mt_redpaint : lm, green);
    for (int s = -1; s <= 1; s += 2)
        mt_obb(ed, x0 + len * 0.5f, y + (float)s * (hw + 1.0f), zt - 22.0f, len, 5.0f, 5.0f, 0, 0, 0, dm, dark);
    for (int k = 0; k < 4; k++)
        mt_obb(ed, x0 + len * (0.18f + 0.21f * (float)k), y, zt + 44.0f, 120.0f, 90.0f, 16.0f, 0, 0, 0, g_mt_steel, dark);
    // the ends: the cab's windshield and lamps (the far end; 2026-11-20: or the near one, the train's end the camera
    // sees), or the gangway's bellows
    for (int e = 0; e < 2; e++) {
        bool is_cab = e == 0 ? cab : cab_near;
        float xe = e == 0 ? x0 + len : x0, o = e == 0 ? 1.0f : -1.0f; // (o: outward along X)
        if (is_cab) {
            mt_obb(ed, xe, y, (zb + zw1) * 0.5f + 10.0f, 4.0f, hw * 2.0f - 4.0f, zw1 - zb + 20.0f, 0, 0, 0, tm, green); // (the end wall)
            mt_obb(ed, xe + o * 1.5f, y - 62.0f, 290.0f, 4.0f, 95.0f, 90.0f, 0, 0, 0, g_mt_glass, dark);
            mt_obb(ed, xe + o * 1.5f, y + 62.0f, 290.0f, 4.0f, 95.0f, 90.0f, 0, 0, 0, g_mt_glass, dark);
            mt_obb(ed, xe + o * 2.5f, y, 200.0f, 6.0f, 60.0f, 170.0f, 0, 0, 0, tm, green); // (the end door)
            for (int s = -1; s <= 1; s += 2)
                mt_obb(ed, xe + o * 3.5f, y + (float)s * 95.0f, 175.0f, 6.0f, 22.0f, 14.0f, 0, 0, 0, g_mt_steel, dark);
            mt_obb(ed, xe + o * 6.0f, y, 120.0f, 14.0f, hw * 2.0f - 20.0f, 30.0f, 0, 0, 0, dm, dark); // (the anticlimber)
            mt_obb(ed, xe + o * 2.0f, y, zw0 - 14.0f, 6.0f, hw * 2.0f, 12.0f, 0, 0, 0, g_mt_redpaint ? g_mt_redpaint : lm, green); // (the stripe round the front)
        } else if (e == 0)
            mt_obb(ed, x0 + len + 30.0f, y, 270.0f, 60.0f, 190.0f, 250.0f, 0, 0, 0, dm, dark);
    }
    // under the floor: the frame, the equipment boxes, the bogies and their wheels
    mt_obb(ed, x0 + len * 0.5f, y, zb - 12.0f, len - 40.0f, hw * 2.0f - 30.0f, 24.0f, 0, 0, 0, dm, dark);
    uint32_t r = seed * 2654435761u + 1u;
    for (int k = 0; k < 5; k++) {
        r = r * 1664525u + 1013904223u;
        float bx = x0 + len * (0.33f + 0.075f * (float)k), bl = 70.0f + (float)(r >> 26);
        mt_obb(ed, bx, y + 30.0f * (float)((r >> 8) & 1), 95.0f, bl, 150.0f, 32.0f, 0, 0, 0, dm, dark);
    }
    for (int bgi = 0; bgi < 2; bgi++) {
        float bx = bgi ? x0 + len - 260.0f : x0 + 260.0f;
        mt_obb(ed, bx, y, 88.0f, 300.0f, 200.0f, 30.0f, 0, 0, 0, dm, dark);
        for (int s = -1; s <= 1; s += 2) {
            mt_obb(ed, bx, y + (float)s * 98.0f, 82.0f, 280.0f, 10.0f, 38.0f, 0, 0, 0, g_mt_steel, dark); // (the side frames)
            for (int a = -1; a <= 1; a += 2) // the wheels on the rails' heads (radius 43: on z 39)
                mt_mesh(ed, SHAPE_CYLINDER, bx + (float)a * 105.0f, y + (float)s * 72.0f, 82.0f, 0, 0, 90.0f, 0.86f, 0.86f, 0.12f, g_mt_steel, dark);
        }
    }
}

static void mt_track(Editor* ed, const char* name, const float (*pts)[3], uint32_t n, uint32_t seed) {
    World* w = &ed->world;
    Entity bed = spline_actor(w, "TrackBed", SPLINE_KIND_SPLINE, (float[3]){ 0, 0, 0 });
    CRail* r = ecs_get(w, bed, COMP_SPLINE);
    r->npoints = n;
    for (uint32_t k = 0; k < n; k++) r->points[k][0] = pts[k][0], r->points[k][1] = MT_Y(pts[k][1]), r->points[k][2] = pts[k][2];
    r->mesh_on = 1, r->mesh = g_mt_bed, r->mesh_axis = 0, r->mesh_length = 200.0f, r->mesh_material = g_mt_gravel;
    r->mesh_color[0] = 0.3f, r->mesh_color[1] = 0.28f, r->mesh_color[2] = 0.26f, r->mesh_roughness = 0.9f;
    Entity tr = spline_actor(w, name, SPLINE_KIND_SPLINE, (float[3]){ 0, 0, 0 });
    r = ecs_get(w, tr, COMP_SPLINE);
    r->npoints = n;
    for (uint32_t k = 0; k < n; k++) r->points[k][0] = pts[k][0], r->points[k][1] = MT_Y(pts[k][1]), r->points[k][2] = pts[k][2];
    r->mesh_on = 1, r->mesh = g_mt_track, r->mesh_axis = 0, r->mesh_length = 200.0f, r->mesh_material = g_mt_rail;
    r->mesh_color[0] = 0.3f, r->mesh_color[1] = 0.25f, r->mesh_color[2] = 0.22f, r->mesh_metallic = 0.8f, r->mesh_roughness = 0.45f;
    // the sleepers: old timber every 65 cm, a little crooked, half sunk in the gravel
    r->inst_on = 1, r->inst_mesh = SHAPE_CUBE, r->inst_material = g_mt_sleeper, r->inst_spacing = 65.0f, r->inst_start = 20.0f;
    r->inst_scale_min = 0.96f, r->inst_scale_max = 1.04f, r->inst_seed = seed, r->inst_jitter = 3.0f, r->inst_yaw_jitter = 3.0f;
    r->inst_scale3[0] = 0.24f, r->inst_scale3[1] = 2.6f, r->inst_scale3[2] = 0.16f, r->inst_up = 16.0f;
    r->inst_align = SPLINE_ALIGN_UPRIGHT;
    r->inst_color[0] = 0.18f, r->inst_color[1] = 0.13f, r->inst_color[2] = 0.09f;
}

// (2026-11-20) The scene's grade as a Color Grading LUT (.cube, 33^3, display space): a soft S curve, the shadows toward
// teal and a little less saturated, the highlights toward warm orange (the reference's cool shade, warm shafts)
static bool mt_write_grade(const char* path) {
    FILE* f = re_fopen(path, "wb");
    if (!f) return false;
    const int N = 33;
    fprintf(f, "TITLE \"MetroGrade\"\nLUT_3D_SIZE %d\n", N);
    for (int b = 0; b < N; b++)
        for (int g = 0; g < N; g++)
            for (int r = 0; r < N; r++) {
                float c[3] = { (float)r / (N - 1), (float)g / (N - 1), (float)b / (N - 1) };
                float lum = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
                float ws = (1.0f - lum) * (1.0f - lum), wh = lum * lum;
                for (int k = 0; k < 3; k++) {
                    float x = c[k];
                    x = x + 0.35f * x * (1.0f - x) * (x - 0.42f) * 2.0f; // (the S: deeper shade, brighter light)
                    x = lum + (x - lum) * (1.0f - 0.18f * ws);          // (the shade a little grayer)
                    static const float cool[3] = { -0.025f, 0.008f, 0.03f }, warm[3] = { 0.045f, 0.012f, -0.035f };
                    x += cool[k] * ws * 0.9f + warm[k] * wh;
                    c[k] = x < 0.0f ? 0.0f : x > 1.0f ? 1.0f : x;
                }
                fprintf(f, "%.5f %.5f %.5f\n", c[0], c[1], c[2]);
            }
    fclose(f);
    return true;
}

static void metro_scene(Editor* ed) {
    World* w = &ed->world;
    if (!project_file()[0]) {
        printf("[RealEngine] FAIL: metro: needs a project (--new-project folder/Name)\n");
        return;
    }
    template_ground(ed, 80.0f);
    editor_sky_preset(ed, 0, 0.0); // (a clear day over the broken roof)
    { // (no clouds: the holes show a patch of sky, and their march costs)
        Entity cl = find_actor(w, COMP_CLOUD, 0);
        if (cl) ecs_destroy(w, cl);
    }
    kit_source(NULL, false); // (the build's DemoContent, 1K)
    // the materials (ambientCG, World Aligned): stained concrete, rusty steel, the train's old green paint, gravel, timber
    g_mt_floor = kit_material("Metro", "Concrete042A", 260.0f, (float[3]){ 0.5f, 0.48f, 0.45f }, 1.0f);
    g_mt_wall = kit_material("Metro", "Concrete042A", 340.0f, (float[3]){ 0.34f, 0.33f, 0.31f }, 1.0f);
    g_mt_steel = kit_material("Metro", "Rust009", 220.0f, (float[3]){ 0.3f, 0.25f, 0.22f }, 1.0f);
    g_mt_rail = kit_material("Metro", "Metal041B", 150.0f, (float[3]){ 0.42f, 0.36f, 0.32f }, 0.8f);
    g_mt_train = kit_material("Metro", "Metal041B", 240.0f, (float[3]){ 0.11f, 0.1f, 0.095f }, 1.0f); // (2026-11-20: darker, browner: old steel in the sun, as the reference's) // (the car: grey steel gone to rust)
    g_mt_train_low = kit_material("Metro", "PaintedMetal006", 260.0f, (float[3]){ 0.26f, 0.18f, 0.18f }, 1.0f); // (its old paint below the windows)
    g_mt_dark = kit_material("Metro", "Rust004", 160.0f, (float[3]){ 0.18f, 0.16f, 0.15f }, 1.0f);
    g_mt_gravel = kit_material("Metro", "Gravel022", 110.0f, (float[3]){ 0.45f, 0.43f, 0.41f }, 1.0f);
    g_mt_sleeper = kit_material("Metro", "Planks023A", 150.0f, (float[3]){ 0.2f, 0.16f, 0.13f }, 1.0f);
    g_mt_roof = kit_material("Metro", "CorrugatedSteel007A", 200.0f, (float[3]){ 0.28f, 0.27f, 0.26f }, 1.0f);
    mt_wet(g_mt_floor, MAT_WET_MASK_ALL, 0.6f, 0.25f, 0.0f);
    mt_wet(g_mt_gravel, MAT_WET_MASK_ALL, 0.6f, 0.15f, 0.0f);
    bool decals = mt_copy_maps("GraffitiSet001") & mt_copy_maps("Leaking006");
    g_mt_glass = mt_material("MetroGlass", mt_glass_build, 0, 1024, 0.0f, false);
    g_mt_black = mt_material("MetroBlack", mt_black_build, 0, 256, 0.0f, false);
    g_mt_puddle = mt_material("MetroPuddle", mt_puddle_build, 0, 512, 0.0f, false);
    g_mt_redpaint = mt_material("MetroRedPaint", mt_redpaint_build, 0, 512, 0.0f, false);
    g_mt_grass = mt_material("MetroGrass", mt_grass_build, 0, 256, 0.0f, true);
    g_mt_leak = decals ? mt_material("MetroLeak", mt_leak_build, 0, 1024, 0.0f, false) : 0;
    static const int tags[6] = { 5, 6, 7, 10, 11, 9 }; // (the warm tags: orange, yellow; one blue)
    for (int k = 0; k < 6; k++) {
        char nm[40];
        snprintf(nm, sizeof nm, "MetroGraffiti%d", k);
        g_mt_graffiti[k] = decals ? mt_material(nm, mt_graffiti_build, tags[k], 1024, 0.0f, false) : 0;
    }
    // the meshes: an I-beam (40 cm deep, 20 wide; 1 m along X), the two rails (UIC 60-like, on a 143.5 cm gauge, 15 cm
    // tall over the sleepers' top at 24 cm), the gravel bed (3.5 m at its foot, 10 cm high), a grass tuft
    {
        static const float ib[12][2] = { { -10, -20 }, { -10, -17 }, { -1, -17 }, { -1, 17 }, { -10, 17 }, { -10, 20 },
                                         { 10, 20 },   { 10, 17 },   { 1, 17 },   { 1, -17 },  { 10, -17 }, { 10, -20 } };
        const int n1 = 12;
        g_mt_ibeam = mt_profile_mesh(ed, "SM_MetroIBeam", ib, &n1, 1, true, 100.0f, 1);
        float rails[24][2];
        static const float rl[12][2] = { { -7, 0 }, { -7, 1.2f }, { -1, 2.5f }, { -0.9f, 10 }, { -3.6f, 11 }, { -3.6f, 15 },
                                         { 3.6f, 15 }, { 3.6f, 11 }, { 0.9f, 10 }, { 1, 2.5f }, { 7, 1.2f }, { 7, 0 } };
        for (int s = 0; s < 2; s++)
            for (int k = 0; k < 12; k++) rails[s * 12 + k][0] = rl[k][0] + (s ? 75.35f : -75.35f), rails[s * 12 + k][1] = rl[k][1] + 24.0f;
        const int n2[2] = { 12, 12 };
        g_mt_track = mt_profile_mesh(ed, "SM_MetroRails", (const float(*)[2])rails, n2, 2, true, 200.0f, 12);
        static const float bd[4][2] = { { -175, 0 }, { -140, 10 }, { 140, 10 }, { 175, 0 } };
        const int n3 = 4;
        g_mt_bed = mt_profile_mesh(ed, "SM_MetroTrackBed", bd, &n3, 1, false, 200.0f, 12);
        g_mt_tuft = mt_tuft_mesh(ed);
        g_mt_ivy = mt_ivy_mesh(ed);
        // (2026-11-20) a metro car's upper shell, 18 m: the sides from the windows' top up, rounded eaves (40 cm), a low
        // barrel roof (18 cm of rise) - the reference's car, where the boxes made a flat crown and two slopes
        {
            float pr[40][2];
            int n = 0;
            const float hw = 140.0f, zw1 = 333.0f, zt = 395.0f, r = 40.0f, rise = 18.0f;
            pr[n][0] = -hw, pr[n][1] = zw1, n++;
            for (int k = 0; k <= 6; k++) { // the left eave: 180 -> 90 degrees around (-hw + r, zt - 20)
                float a = 3.14159265f * (1.0f - 0.5f * (float)k / 6.0f);
                pr[n][0] = -hw + r + r * cosf(a), pr[n][1] = zt - 20.0f + r * sinf(a), n++;
            }
            for (int k = 1; k < 10; k++) { // the barrel
                float yy = -(hw - r) + 2.0f * (hw - r) * (float)k / 10.0f, q = yy / (hw - r);
                pr[n][0] = yy, pr[n][1] = zt + 20.0f + rise * (1.0f - q * q), n++;
            }
            for (int k = 0; k <= 6; k++) { // the right eave: 90 -> 0
                float a = 3.14159265f * 0.5f * (1.0f - (float)k / 6.0f);
                pr[n][0] = hw - r + r * cosf(a), pr[n][1] = zt - 20.0f + r * sinf(a), n++;
            }
            pr[n][0] = hw, pr[n][1] = zw1, n++;
            g_mt_carroof = mt_profile_mesh(ed, "SM_MetroCarRoof", (const float(*)[2])pr, &n, 1, false, 1800.0f, 9);
        }
    }
    const float concrete[3] = { 0.32f, 0.31f, 0.3f }, rust[3] = { 0.2f, 0.12f, 0.08f };
    // the hall: 64 m along X (x -1200 .. 5200), 17 m wide (the right wall at y -700, the left at 1000), 11.5 m high
    Entity floor_tpl = ENTITY_NONE;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) { // (the template's ground: under the hall, out of sight)
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_STATIC_MESH)) || !(a->mask & COMP_BIT(COMP_NAME))) continue;
        const CName* nm = ecs_arch_column(a, COMP_NAME);
        for (uint32_t i = 0; i < a->count; i++)
            if (!strcmp(nm[i].text, "Floor")) floor_tpl = ecs_arch_entities(a)[i];
    }
    if (floor_tpl) ecs_destroy(w, floor_tpl);
    mt_box(ed, -1300, -760, -40, 5300, 1060, 0.0f, g_mt_floor, concrete);
    mt_box(ed, -1300, -760, 0, 5300, -700, 1160, g_mt_wall, concrete);   // the right wall
    // the left wall; (2026-11-20) a broken high window in it over the camera's left (x -700..300, z 7.2-10.8 m) with
    // its steel mullions: the sun from the left falls through it onto the rubble in the foreground, as the reference's
    mt_box(ed, -1300, 1000, 0, -700, 1060, 1160, g_mt_wall, concrete);
    mt_box(ed, -700, 1000, 0, 300, 1060, 720, g_mt_wall, concrete);
    mt_box(ed, -700, 1000, 1080, 300, 1060, 1160, g_mt_wall, concrete);
    mt_box(ed, 300, 1000, 0, 5300, 1060, 1160, g_mt_wall, concrete);
    for (float x = -620.0f; x < 300.0f; x += 115.0f)
        mt_box(ed, x - 5.0f, 1020, 720, x + 5.0f, 1035, 1080, g_mt_dark, (float[3]){ 0.18f, 0.12f, 0.08f });
    mt_box(ed, -700, 1015, 895, 300, 1040, 905, g_mt_dark, (float[3]){ 0.18f, 0.12f, 0.08f }); // (the transom)
    mt_box(ed, -1300, -700, 0, -1200, 1000, 1160, g_mt_wall, concrete);  // behind the camera
    // the far wall and its tunnel mouth (5 x 6 m, an arch: the black beyond)
    mt_box(ed, 5200, -700, 0, 5300, -500, 1160, g_mt_wall, concrete);
    mt_box(ed, 5200, 500, 0, 5300, 1000, 1160, g_mt_wall, concrete);
    mt_box(ed, 5200, -500, 760, 5300, 500, 1160, g_mt_wall, concrete);
    for (int k = 0; k < 6; k++) { // (the arch's haunches: steps of the wall into the opening's top corners)
        float a0 = (float)k / 6.0f * 1.5707963f, dy = 500.0f * (1.0f - cosf(a0)), z0 = 520.0f + 240.0f * sinf(a0);
        mt_box(ed, 5200, -500, z0, 5300, -500 + dy + 10.0f, 760, g_mt_wall, concrete);
        mt_box(ed, 5200, 500 - dy - 10.0f, z0, 5300, 500, 760, g_mt_wall, concrete);
    }
    mt_box(ed, 5300, -520, -10, 7300, 520, 790, g_mt_black, (float[3]){ 0.004f, 0.004f, 0.004f });
    // (2026-11-20) a cross wall 43 m ahead with two arches (track A's, 6 m; track B's, 4.8 m) and a walkway along its
    // face with a railing: the reference's arch in the middle of the view, its upper level. The arch's top in 14 slabs,
    // each as wide as the circle leaves it; the hall beyond (no hole in its roof) dark, the far tunnel's light deep in it
    {
        const float wx0 = 2350.0f, wx1 = 2450.0f; // (the train goes through arch B: the second car beyond it, lit by the far hole)
        static const float ar[2][3] = { { 367.0f, 300.0f, 330.0f }, { MT_TRACK_B, 240.0f, 420.0f } }; // (center y, radius, spring z)
        // the piers: right wall .. arch B .. arch A .. left wall
        mt_box(ed, wx0, -700, 0, wx1, ar[1][0] - ar[1][1], 1160, g_mt_wall, concrete);
        mt_box(ed, wx0, ar[1][0] + ar[1][1], 0, wx1, ar[0][0] - ar[0][1], 1160, g_mt_wall, concrete);
        mt_box(ed, wx0, ar[0][0] + ar[0][1], 0, wx1, 1000, 1160, g_mt_wall, concrete);
        for (int a = 0; a < 2; a++) {
            float cy = ar[a][0], r = ar[a][1], zs = ar[a][2];
            const int N = 14;
            for (int j = 0; j < N; j++) {
                float z0 = zs + r * (float)j / N, z1 = zs + r * (float)(j + 1) / N, zm = (z0 + z1) * 0.5f - zs;
                float hw = sqrtf(fmaxf(r * r - zm * zm, 0.0f));
                mt_box(ed, wx0, cy - r, z0, wx1, cy - hw, z1, g_mt_wall, concrete);
                mt_box(ed, wx0, cy + hw, z0, wx1, cy + r, z1, g_mt_wall, concrete);
            }
            mt_box(ed, wx0, cy - r, zs + r, wx1, cy + r, 1160, g_mt_wall, concrete); // (over the crown)
            mt_box(ed, wx0 - 6.0f, cy - r - 30.0f, zs - 12.0f, wx0, cy - r + 6.0f, zs + 4.0f, g_mt_wall, concrete); // (the imposts)
            mt_box(ed, wx0 - 6.0f, cy + r - 6.0f, zs - 12.0f, wx0, cy + r + 30.0f, zs + 4.0f, g_mt_wall, concrete);
        }
        // the walkway: a slab along the wall's face at 7.6 m, its edge beam, posts every 1.5 m and two rails (old orange)
        const float orange[3] = { 0.55f, 0.18f, 0.05f };
        mt_box(ed, wx0 - 160.0f, -700, 760, wx0, 1000, 784, g_mt_floor, concrete);
        mt_box(ed, wx0 - 172.0f, -700, 730, wx0 - 156.0f, 1000, 786, g_mt_steel, rust);
        for (float yy = -650.0f; yy < 1000.0f; yy += 150.0f)
            mt_box(ed, wx0 - 168.0f, yy - 3.0f, 784, wx0 - 162.0f, yy + 3.0f, 890, g_mt_redpaint ? g_mt_redpaint : g_mt_steel, orange);
        mt_box(ed, wx0 - 169.0f, -700, 884, wx0 - 161.0f, 1000, 892, g_mt_redpaint ? g_mt_redpaint : g_mt_steel, orange);
        mt_box(ed, wx0 - 168.0f, -700, 834, wx0 - 162.0f, 1000, 839, g_mt_redpaint ? g_mt_redpaint : g_mt_steel, orange);
    }
    // the left platform (90 cm, its edge worn), pilasters along both walls
    mt_box(ed, -1200, 700, 0, 5200, 1000, 90, g_mt_wall, concrete);
    mt_box(ed, -1200, 690, 82, 5200, 712, 96, g_mt_floor, concrete);
    for (float x = -900.0f; x <= 4900.0f; x += 600.0f) {
        mt_box(ed, x - 40, -700, 0, x + 40, -660, 1160, g_mt_wall, concrete);
        mt_box(ed, x - 40, 960, 0, x + 40, 1000, 1160, g_mt_wall, concrete);
    }
    // the steel: columns on the platform's edge and along the right wall, the roof's girders across, the purlins along,
    // the knee braces; the roof of corrugated sheets broken open in two places (the sun comes in there)
    for (float x = -900.0f; x <= 4900.0f; x += 600.0f) {
        // (2026-11-20) the platform's columns built up of plates as the reference's (a 70 x 50 cm box, angles proud at
        // its corners, a batten every 1.5 m): riveted steel read from afar; the I-beam inside it carries the braces
        if (x < 2300.0f) {
            mt_box(ed, x - 35.0f, 615.0f, 0, x + 35.0f, 665.0f, 1120.0f, g_mt_steel, rust);
            for (int c = 0; c < 4; c++) {
                float cx = (c & 1) ? x + 33.0f : x - 37.0f, cyy = (c & 2) ? 661.0f : 613.0f;
                mt_box(ed, cx, cyy, 0, cx + 4.0f, cyy + 6.0f, 1120.0f, g_mt_dark, rust);
            }
            for (float z = 110.0f; z < 1100.0f; z += 150.0f)
                mt_box(ed, x - 39.0f, 611.0f, z, x + 39.0f, 669.0f, z + 14.0f, g_mt_steel, rust);
            mt_box(ed, x - 50.0f, 600.0f, 90.0f, x + 50.0f, 680.0f, 112.0f, g_mt_dark, rust); // (its base plate on the platform)
        }
        mt_mesh(ed, g_mt_ibeam, x, 640.0f, 560.0f, 90.0f, 0.0f, 0.0f, 11.2f, 1.6f, 1.6f, g_mt_steel, rust);
        mt_mesh(ed, g_mt_ibeam, x, -630.0f, 560.0f, 90.0f, 0.0f, 0.0f, 11.2f, 1.3f, 1.3f, g_mt_steel, rust);
        mt_mesh(ed, g_mt_ibeam, x, 150.0f, 1110.0f, 0.0f, 90.0f, 0.0f, 17.0f, 1.2f, 1.6f, g_mt_steel, rust); // (across)
        for (int s = -1; s <= 1; s += 2) { // (the knee braces from the columns up to the girder)
            float cy = s > 0 ? 640.0f : -630.0f;
            mt_mesh(ed, g_mt_ibeam, x, cy - (float)s * 110.0f, 980.0f, -(float)s * 45.0f, 90.0f, 0.0f, 3.1f, 0.7f, 0.7f, g_mt_steel, rust);
        }
    }
    for (int k = -1; k <= 1; k++) // (the purlins along the hall)
        mt_mesh(ed, g_mt_ibeam, 2000.0f, 150.0f + (float)k * 480.0f, 1060.0f, 0.0f, 0.0f, 0.0f, 64.0f, 0.8f, 1.0f, g_mt_steel, rust);
    // the roof sheets: whole but for two holes (HA: x 300..1900, y -200..520 over the camera's way and the train; HB: x
    // 2700..3300, y -50..450), a sheet a box between the roof's ribs (the World Aligned steel tiles over them alike)
    const float rz0 = 1140.0f, rz1 = 1160.0f;
    static const float HA[4] = { -600, 1900, -200, 760 }, HB[4] = { 2700, 3300, -50, 450 };
    mt_box(ed, -1200, -700, rz0, HA[0], 1000, rz1, g_mt_roof, concrete);
    mt_box(ed, HA[1], -700, rz0, HB[0], 1000, rz1, g_mt_roof, concrete);
    mt_box(ed, HB[1], -700, rz0, 5200, 1000, rz1, g_mt_roof, concrete);
    mt_box(ed, HA[0], -700, rz0, HA[1], HA[2], rz1, g_mt_roof, concrete);
    mt_box(ed, HA[0], HA[3], rz0, HA[1], 1000, rz1, g_mt_roof, concrete);
    mt_box(ed, HB[0], -700, rz0, HB[1], HB[2], rz1, g_mt_roof, concrete);
    mt_box(ed, HB[0], HB[3], rz0, HB[1], 1000, rz1, g_mt_roof, concrete);
    // inside the holes the roof's lattice is left (rafters across every 1.4 m, purlins along) and most of its sheets:
    // the gaps run along the hall, narrow across it, so the sun (from the left, high) falls through them as separate
    // sheets of light seen edge on, slanting down to the train as in the reference
    for (float x = HA[0] + 70.0f; x < HA[1]; x += 140.0f)
        mt_mesh(ed, g_mt_ibeam, x, (HA[2] + HA[3]) * 0.5f, 1125.0f, 0.0f, 90.0f, 0.0f, (HA[3] - HA[2]) / 100.0f, 0.5f, 0.6f, g_mt_steel, rust);
    for (float x = HB[0] + 70.0f; x < HB[1]; x += 140.0f)
        mt_mesh(ed, g_mt_ibeam, x, (HB[2] + HB[3]) * 0.5f, 1125.0f, 0.0f, 90.0f, 0.0f, (HB[3] - HB[2]) / 100.0f, 0.5f, 0.6f, g_mt_steel, rust);
    for (int k = 0; k < 2; k++) // (two purlins along through the hole)
        mt_mesh(ed, g_mt_ibeam, (HA[0] + HA[1]) * 0.5f, k ? 520.0f : 200.0f, 1128.0f, 0.0f, 0.0f, 0.0f, (HA[1] - HA[0]) / 100.0f, 0.5f, 0.5f, g_mt_steel, rust);
    // the sheets: all but the gaps (x0, x1, y0, y1; each hole's in rising y, apart along y)
    static const float gaps[7][4] = { { 200, 1300, 80, 160 },   { -500, 1880, 240, 330 }, { -300, 1500, 420, 520 }, { 400, 1750, 560, 650 },
                                      { -550, 1850, 680, 750 }, { 2750, 3200, 100, 170 }, { 2700, 3300, 300, 380 } };
    for (int h = 0; h < 2; h++) {
        const float* H = h ? HB : HA;
        float ya = H[2];
        for (int k = 0; k <= 7; k++) {
            bool in = k < 7 && gaps[k][0] >= H[0] && gaps[k][1] <= H[1] && gaps[k][2] >= H[2] && gaps[k][3] <= H[3];
            if (k < 7 && !in) continue;
            float yb = k < 7 ? gaps[k][2] : H[3];
            if (yb > ya) mt_box(ed, H[0], ya, rz0, H[1], yb, rz1, g_mt_roof, concrete);
            if (k < 7) { // (beside the gap, along x)
                if (gaps[k][0] > H[0]) mt_box(ed, H[0], gaps[k][2], rz0, gaps[k][0], gaps[k][3], rz1, g_mt_roof, concrete);
                if (gaps[k][1] < H[1]) mt_box(ed, gaps[k][1], gaps[k][2], rz0, H[1], gaps[k][3], rz1, g_mt_roof, concrete);
                ya = gaps[k][3];
            }
        }
    }
    // (2026-11-20) planks and leftover sheets across each gap, at uneven steps: a gap's long sheet of light breaks into
    // separate shafts (the reference's several), and the train's side gets patches of sun instead of a lit band
    {
        uint32_t rr = 4242u;
#define MT_R(a, b) ((rr = rr * 1664525u + 1013904223u), (a) + ((b) - (a)) * (float)(rr >> 8) / 16777216.0f)
        for (int k = 0; k < 7; k++)
            for (float x = gaps[k][0] + MT_R(40.0f, 220.0f); x < gaps[k][1] - 40.0f;) {
                float w = MT_R(45.0f, 150.0f);
                mt_box(ed, x, gaps[k][2] - 4.0f, rz0 - 3.0f, fminf(x + w, gaps[k][1]), gaps[k][3] + 4.0f, rz1 - 6.0f, g_mt_roof, concrete);
                x += w + MT_R(110.0f, 330.0f);
            }
#undef MT_R
    }
    // broken sheets hanging into the holes and fallen on the floor
    mt_obb(ed, -150.0f, 380.0f, 1075.0f, 140.0f, 280.0f, 3.0f, 38.0f, 5.0f, 0.0f, g_mt_roof, concrete);
    mt_obb(ed, 1700.0f, 400.0f, 1060.0f, 120.0f, 240.0f, 3.0f, -42.0f, 10.0f, 15.0f, g_mt_roof, concrete);
    mt_obb(ed, 2900.0f, 260.0f, 1090.0f, 100.0f, 220.0f, 3.0f, 0.0f, 0.0f, 38.0f, g_mt_roof, concrete);
    mt_obb(ed, 1250.0f, 380.0f, 6.0f, 150.0f, 260.0f, 3.0f, 0.0f, 25.0f, 4.0f, g_mt_roof, concrete);
    mt_obb(ed, 2350.0f, 560.0f, 30.0f, 180.0f, 220.0f, 3.0f, 0.0f, -15.0f, 18.0f, g_mt_roof, concrete);
    // the tracks: B straight under the train, A from beside the camera curving left into the tunnel
    static const float tb[3][3] = { { -1200, MT_TRACK_B, 0 }, { 2000, MT_TRACK_B, 0 }, { 5600, MT_TRACK_B, 0 } };
    mt_track(ed, "TrackB", tb, 3, 11);
    static const float ta[6][3] = { { -1200, 250, 0 }, { 400, 250, 0 }, { 1800, 334, 0 }, { 3000, 406, 0 }, { 4300, 484, 0 }, { 5600, 562, 0 } };
    mt_track(ed, "TrackA", ta, 6, 23);
    // the train: two cars on track B, the far one's cab facing the tunnel
    mt_car(ed, -350.0f, 1800.0f, false, true, 1); // (2026-11-20: its near end a cab, closed: the camera sees it)
    mt_car(ed, 1510.0f, 1800.0f, true, false, 2);
    // graffiti on the cars' sides (Decals projected along -y onto the side facing the camera) and rust under the windows
    if (decals) {
        static const float gf[8][4] = { { -90, 180, 120, 0 }, { 290, 185, 140, 1 }, { 710, 175, 110, 2 }, { 1190, 190, 150, 3 },
                                        { 1790, 180, 130, 4 }, { 2250, 185, 140, 5 }, { 2740, 178, 120, 0 }, { 470, 380, 90, 2 } };
        for (int k = 0; k < 8; k++)
            mt_decal(w, g_mt_graffiti[(int)gf[k][3]], gf[k][0], MT_TRACK_B + 140.0f, gf[k][1], 0, -90, 0, (float[3]){ 25.0f, gf[k][2], gf[k][2] * 0.5f });
        for (float x = -280.0f; x < 3300.0f; x += 150.0f)
            if (fmodf(x * 7.3f, 5.0f) < 3.2f)
                mt_decal(w, g_mt_leak, x, MT_TRACK_B + 140.0f, 200.0f, 0, -90, 0, (float[3]){ 25.0f, 22.0f, 70.0f });
    }
    // puddles: on the concrete beside and between the tracks, the most where the roof is open (the rain comes in there)
    {
        uint32_t r = 977u;
        int made = 0;
        for (int k = 0; k < 160 && made < 46; k++) {
            r = r * 1664525u + 1013904223u;
            float x = -600.0f + (float)(r >> 8) / 16777216.0f * 5400.0f;
            r = r * 1664525u + 1013904223u;
            float yy = -650.0f + (float)(r >> 8) / 16777216.0f * 1330.0f;
            r = r * 1664525u + 1013904223u;
            float s = 50.0f + (float)(r >> 8) / 16777216.0f * 180.0f;
            float ya = mt_track_a_y(x);
            bool on_a = fabsf(yy - ya) < 175.0f, on_b = fabsf(yy - MT_TRACK_B) < 175.0f;
            if (on_b && x > -400.0f && x < 3400.0f) continue; // (under the train: unseen)
            float z = (on_a || on_b) ? 9.0f : 0.0f;
            mt_decal(w, g_mt_puddle, x, yy, z, -90.0f, 0.0f, (float)(r & 255) * 1.4f,
                     (float[3]){ (on_a || on_b) ? 4.0f : 6.0f, s * (0.8f + 0.4f * (float)((r >> 4) & 15) / 15.0f), s });
            made++;
        }
        for (int k = 0; k < 7; k++) // (and a few big ones in the sun's patch, in front of the camera)
            mt_decal(w, g_mt_puddle, -150.0f + 190.0f * (float)k, -90.0f + 110.0f * sinf((float)k * 1.7f), 0.0f, -90.0f, 0.0f, 25.0f * (float)k,
                     (float[3]){ 6.0f, 150.0f + 30.0f * (float)(k % 3), 110.0f });
        // right in front of the camera, on the floor and between the rails: they mirror the roof's gaps and the shafts
        // (a puddle a few meters ahead sees the roof right above the camera)
        // (2026-11-20: and two wide ones on track A's bed ahead, where the reference's floor shines: the shafts in them)
        static const float near_p[8][4] = { { -150, -40, 0, 140 }, { 150, 20, 0, 120 }, { 420, -60, 0, 150 },
                                            { -300, 250, 9, 70 }, { 0, 250, 9, 60 },    { 260, 255, 9, 65 },
                                            { 120, 240, 9, 170 }, { 520, 225, 9, 210 } };
        for (int k = 0; k < 8; k++)
            mt_decal(w, g_mt_puddle, near_p[k][0], near_p[k][1], near_p[k][2], -90.0f, 0.0f, 40.0f * (float)k,
                     (float[3]){ near_p[k][2] > 0.0f ? 30.0f : 6.0f, near_p[k][3] * 1.3f, near_p[k][3] }); // (on a track: deep enough for the gravel and the sleepers' tops)
    }
    // junk: tyres, barrels, boxes, bags, branches, fallen concrete
    mt_props(ed, "old_tyre", 3, (const float[][5]){ { 700, 420, 0, 30, 1 }, { 2600, -560, 0, 80, 1 }, { 760, 470, 18, 50, 1 } }, NULL, NULL);
    mt_props(ed, "barrel_03", 3, (const float[][5]){ { 180, 560, 0, 0, 1 }, { 3800, 600, 0, 40, 1 }, { 240, 600, 0, 70, 1 } }, NULL, NULL);
    mt_props(ed, "cardboard_box_01", 2, (const float[][5]){ { 1150, 600, 0, 15, 1 }, { 4200, -600, 0, 50, 1 } }, NULL, NULL);
    mt_props(ed, "trashbag", 2, (const float[][5]){ { 420, 610, 0, 0, 1 }, { 2200, 640, 90, 40, 1 } }, NULL, NULL);
    mt_props(ed, "dry_branches_medium_01", 3, (const float[][5]){ { 900, 260, 9, 20, 1 }, { 1900, 170, 9, 140, 1 }, { 2800, 540, 0, 70, 1 } }, NULL, NULL);
    for (int k = 0; k < 14; k++) { // (chunks of concrete fallen from the roof's edge)
        float a = (float)k * 2.39996f, d = 60.0f + 25.0f * (float)(k % 5);
        float s = 12.0f + 7.0f * (float)(k % 4);
        mt_obb(ed, 1250.0f + cosf(a) * d * 3.0f, 250.0f + sinf(a) * d * 2.0f, s * 0.4f, s, s * 0.8f, s * 0.6f, (float)(k * 23 % 40), (float)(k * 57),
               (float)(k * 31 % 30), g_mt_wall, concrete);
    }
    // (2026-11-20) heaps of rubble as the reference's foreground (left, by the platform and on it), smaller ones along
    // the tracks; ivy hanging from the columns' tops, over the arches and from the train's eaves
    mt_rubble(ed, -480.0f, 600.0f, 0.0f, 190.0f, 34, 1);
    mt_rubble(ed, -250.0f, 820.0f, 90.0f, 150.0f, 22, 2);
    mt_rubble(ed, 200.0f, 590.0f, 0.0f, 140.0f, 22, 3);
    mt_rubble(ed, 950.0f, 560.0f, 0.0f, 120.0f, 16, 4);
    mt_rubble(ed, 380.0f, -40.0f, 0.0f, 90.0f, 12, 5);
    mt_rubble(ed, 1700.0f, 600.0f, 0.0f, 110.0f, 14, 6);
    if (g_mt_ivy) {
        const float leaf[3] = { 0.16f, 0.24f, 0.06f };
        for (float x = -900.0f; x < 2300.0f; x += 600.0f) { // (from the columns' tops, facing the camera: -X)
            mt_mesh(ed, g_mt_ivy, x - 40.0f, 640.0f, 1080.0f, 0, 180.0f, 0, 1.0f, 1.0f, 1.0f + 0.3f * sinf(x), g_mt_grass, leaf);
            mt_mesh(ed, g_mt_ivy, x - 40.0f, -630.0f, 1000.0f, 0, 180.0f, 0, 0.8f, 1.0f, 0.8f, g_mt_grass, leaf);
        }
        for (int k = 0; k < 5; k++) // (over the arches, on the wall's face)
            mt_mesh(ed, g_mt_ivy, 2340.0f, -500.0f + 300.0f * (float)k, 1130.0f, 0, 180.0f, 0, 1.3f, 1.4f, 1.2f, g_mt_grass, leaf);
        for (int k = 0; k < 4; k++) // (from the first car's eaves, over its windows, as the reference's vines)
            mt_mesh(ed, g_mt_ivy, -200.0f + 420.0f * (float)k, MT_TRACK_B + 145.0f, 430.0f, 0, -90.0f, 0, 0.9f, 1.0f, 0.35f, g_mt_grass, leaf);
    }
    // the plants: weeds, ferns, shrubs and grass (a Foliage actor: grass tufts and weeds painted where the sun falls,
    // along the tracks and the walls), bigger shrubs placed by hand
    uint32_t weed_mesh = 0, weed_mat = 0, fern_mesh = 0, fern_mat = 0;
    mt_props(ed, "weed_plant_02", 0, NULL, &weed_mesh, &weed_mat);
    mt_props(ed, "fern_02", 0, NULL, &fern_mesh, &fern_mat);
    // (2026-11-20) more shrubs and dry branches by hand: on the rubble heaps, by the platform, between the tracks
    mt_props(ed, "shrub_04", 6, (const float[][5]){ { -520, 640, 40, 20, 0.7f }, { -380, 520, 20, 110, 0.6f }, { 180, 640, 30, 60, 0.6f }, { -200, 860, 120, 150, 0.7f },
                                                    { 760, 470, 0, 30, 0.5f }, { 1500, 620, 0, 200, 0.6f } }, NULL, NULL);
    mt_props(ed, "dry_branches_medium_01", 4, (const float[][5]){ { -600, 470, 0, 60, 1.0f }, { 120, 120, 9, 10, 0.9f }, { 600, 660, 0, 130, 1.0f }, { -100, 560, 30, 200, 0.8f } }, NULL, NULL);
    mt_props(ed, "shrub_03", 4, (const float[][5]){ { 1300, 640, 90, 0, 1.0f }, { 1700, 760, 90, 100, 1.2f }, { 2950, 680, 90, 200, 1.0f }, { 600, -640, 0, 50, 0.9f } }, NULL, NULL);
    mt_props(ed, "shrub_04", 3, (const float[][5]){ { 1050, 520, 0, 40, 0.9f }, { 3100, 420, 0, 160, 0.8f }, { 4400, -620, 0, 90, 0.8f } }, NULL, NULL);
    Entity fol = spawn_actor(ed, SPAWN_FOLIAGE, 0, (float[3]){ 0, 0, 0 }, 0);
    CFoliage* fc = fol ? ecs_get(w, fol, COMP_FOLIAGE) : NULL;
    if (fc) {
        snprintf(fc->name, sizeof fc->name, "MetroFoliage");
        memset(fc->types, 0, sizeof fc->types);
        fc->ntypes = 0;
        const struct { uint32_t mesh, mat; float density, smin, smax, wind; float col[3]; } ty[FOLIAGE_TYPES] = { // (4 kinds at most: the component's)
            { g_mt_tuft, g_mt_grass, 550.0f, 0.6f, 1.25f, 0.25f, { 0.2f, 0.26f, 0.07f } },
            { weed_mesh, weed_mat, 70.0f, 1.8f, 3.0f, 0.15f, { 0.2f, 0.3f, 0.1f } }, // (a rosette 30 cm wide at 1)
            { fern_mesh, fern_mat, 40.0f, 0.6f, 1.1f, 0.1f, { 0.2f, 0.3f, 0.1f } },
            { g_mt_tuft, g_mt_grass, 220.0f, 0.5f, 1.0f, 0.25f, { 0.36f, 0.3f, 0.12f } }, // (2026-11-20: dry, yellowed grass)
        };
        for (int k = 0; k < FOLIAGE_TYPES; k++) {
            if (!ty[k].mesh) continue;
            FoliageType* t = &fc->types[fc->ntypes++];
            t->mesh = ty[k].mesh, t->material = ty[k].mat, memcpy(t->color, ty[k].col, 12);
            t->density = ty[k].density, t->scale[0] = ty[k].smin, t->scale[1] = ty[k].smax, t->stretch = 1.0f, t->slope = 30.0f;
            t->wind = ty[k].wind, t->paint = 1;
        }
        fc->seed = 7;
        foliage_update(w); // (its instances' store made now: painted below, saved with the level's Foliage file)
        foliage_clear(w, fol, -1);
        // where the sun falls (lush), between and beside track A (sparse), along the tracks' outer edges, the platform's
        // edge and the right wall
        for (int k = 0; k < 9; k++)
            mt_paint(w, fol, 150.0f + 180.0f * (float)k, -60.0f + 70.0f * sinf((float)k), 0.0f, 170.0f, 0.8f);
        for (float x = -500.0f; x < 5000.0f; x += 180.0f) {
            float ya = mt_track_a_y(x);
            mt_paint(w, fol, x, ya, 10.0f, 55.0f, 0.35f);
            mt_paint(w, fol, x, ya + 205.0f, 0.0f, 50.0f, 0.6f);
            mt_paint(w, fol, x, ya - 205.0f, 0.0f, 45.0f, 0.4f);
            mt_paint(w, fol, x + 60.0f, 680.0f, 0.0f, 50.0f, 0.7f);
            mt_paint(w, fol, x + 90.0f, -680.0f, 0.0f, 35.0f, 0.5f);
            mt_paint(w, fol, x + 30.0f, 860.0f, 90.0f, 80.0f, 0.5f);
        }
        foliage_stroke_end(w, fol);
        printf("[RealEngine] Metro: %u plants painted (grass %u, weeds %u, ferns %u)\n", foliage_count(fol, -1), foliage_count(fol, 0),
               foliage_count(fol, 1), foliage_count(fol, 2));
        if (foliage_count(fol, -1) < 500 || foliage_count(fol, 1) == 0)
            printf("[RealEngine] FAIL: metro: the Foliage painted %u plants (weeds %u)\n", foliage_count(fol, -1), foliage_count(fol, 1));
    }
    // the dust in the shafts (motes drifting in the light)
    {
        char vp[200], dir[700];
        snprintf(vp, sizeof vp, "/Content/Particles/PS_Dust.rparticle");
        snprintf(dir, sizeof dir, "%s/Particles", content_root());
        re_make_dirs(dir);
        ParticleSystemAsset* pa = ps_load(vp);
        uint32_t h = pa ? pa->hash : ps_create("/Content/Particles", "PS_Dust", PST_DUST);
        if (h) {
            particles_spawn(w, h, (float[3]){ 1200.0f, MT_Y(0.0f), 300.0f }, NULL, false);
            particles_spawn(w, h, (float[3]){ 2500.0f, MT_Y(100.0f), 400.0f }, NULL, false);
        }
    }
    // the light: the sun high and warm through the holes, slanting toward the camera and the train; the sky light; a
    // dense dusty air for the shafts (volumetric fog); a fixed exposure for the dark hall; the grade (warm light, cool
    // shade, a vignette, a little grain)
    Entity sun = find_actor(w, COMP_LIGHT, LIGHT_DIRECTIONAL);
    CTransform* st = sun ? ecs_get(w, sun, COMP_TRANSFORM) : NULL;
    CLight* sl = sun ? ecs_get(w, sun, COMP_LIGHT) : NULL;
    if (st) st->rotation[0] = -60.0f, st->rotation[1] = MT_Y(-118.0f), st->rotation[2] = 0.0f; // (2026-11-20: a little more toward the camera: the shafts brighter, still onto the train)
    if (sl) sl->color[0] = 1.0f, sl->color[1] = 0.78f, sl->color[2] = 0.52f, sl->intensity = 110000.0f, sl->source_radius = 0.0f;
    hz_light(ed, "TunnelLight", 5600.0f, MT_Y(0.0f), 380.0f, 1.0f, 0.82f, 0.62f, 4000.0f, 1500.0f, false); // (daylight past the far arch)
    Entity sky = find_actor(w, COMP_LIGHT, LIGHT_SKY);
    CLight* skl = sky ? ecs_get(w, sky, COMP_LIGHT) : NULL;
    if (skl) skl->intensity = 0.3f; // (the sky only through the holes: a dim, cool fill)
    Entity fog = find_actor(w, COMP_FOG, 0);
    CFog* fg = fog ? ecs_get(w, fog, COMP_FOG) : NULL;
    if (fg) {
        fg->density = 0.009f, fg->height_falloff = 0.02f, fg->anisotropy = 0.8f, fg->volumetric = 1, fg->volumetric_distance = 6500.0f; // (2026-11-20: 35 m made the far end's analytic fog glow, unshadowed)
        fg->albedo[0] = 0.86f, fg->albedo[1] = 0.9f, fg->albedo[2] = 0.96f;
    }
    Entity pp = spawn_actor(ed, SPAWN_POST, 0, (float[3]){ 0, 0, 0 }, 0.0);
    CPostProcess* pv = pp ? ecs_get(w, pp, COMP_POST_PROCESS) : NULL;
    if (pv) {
        pv->flags = PPV_ENABLED | PPV_UNBOUND;
        // (2026-11-20: a little brighter, the bounce stronger: the reference's left side reads, columns and rubble)
        const int fs[] = { PP_EXPOSURE_MIN_EV, PP_EXPOSURE_MAX_EV, PP_VIGNETTE, PP_GRAIN, PP_SATURATION, PP_CONTRAST, PP_BLOOM_INTENSITY, PP_GI_INTENSITY };
        const float vs[] = { 8.7f, 8.7f, 0.45f, 0.07f, 1.0f, 1.05f, 0.8f, 1.8f };
        for (int k = 0; k < 8; k++) pv->overrides |= 1u << fs[k], pv->s.v[fs[k]] = vs[k];
        char lp[1200]; // (2026-11-20) the grade: teal shade, warm shafts (mt_write_grade)
        snprintf(lp, sizeof lp, "%s/Demo/Metro/MetroGrade.cube", content_root());
        if (mt_write_grade(lp)) snprintf(pv->lut, sizeof pv->lut, "/Content/Demo/Metro/MetroGrade.cube"), pv->lut_intensity = 1.0f;
    }
    // the player's start (Play) and the editor's view: the reference's framing (beside track A, the train on the right)
    Entity ps = find_actor(w, COMP_PLAYER_START, 0);
    CTransform* pst = ps ? ecs_get(w, ps, COMP_TRANSFORM) : NULL;
    if (pst) pst->location[0] = -450, pst->location[1] = MT_Y(250), pst->location[2] = 100, pst->rotation[1] = MT_Y(-5);
    ed->selected = ENTITY_NONE;
    // (2026-11-20) the reference's lens: low (1.3 m), a little up, wide (95 degrees)
    ed->cam.pos[0] = -850.0f, ed->cam.pos[1] = MT_Y(310.0f), ed->cam.pos[2] = 130.0f; // (back to the hall's end: the train smaller, as in the reference)
    ed->cam.yaw = MT_Y(-5.0f), ed->cam.pitch = 7.0f, ed->cam.fov_deg = 95.0f;
    memcpy(ed->pie_cam.pos, ed->cam.pos, 12), ed->pie_cam.yaw = ed->cam.yaw, ed->pie_cam.pitch = ed->cam.pitch, ed->pie_cam.fov_deg = ed->cam.fov_deg;
    // the level into the project, its startup map
    ProjectSettings* prj = project();
    char path[1200];
    snprintf(path, sizeof path, "%s/Maps", content_root());
    re_make_dirs(path);
    snprintf(path, sizeof path, "%s/Maps/MetroAbandonado.rlevel", content_root());
    if (editor_save_level(ed, path, re_time_seconds())) {
        snprintf(prj->editor_startup_map, sizeof prj->editor_startup_map, "/Content/Maps/MetroAbandonado.rlevel");
        snprintf(prj->game_default_map, sizeof prj->game_default_map, "/Content/Maps/MetroAbandonado.rlevel");
        prj->game_scalability = 2; // (High)
        project_save(NULL);
        printf("[RealEngine] Metro saved: %s\n", path);
    } else printf("[RealEngine] FAIL: metro: the level not saved\n");
    ed->dirty = false;
}
