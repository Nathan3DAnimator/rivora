// The demos' kit (sugestões #29/#31/#34/#43; included by editor.c before demo_games.inc): the CC0 assets the build put
// in DemoContent next to the editor (tools/fetch_demo_assets.cmake: Poly Haven models, ambientCG textures; their
// authors and licenses in DemoContent/LICENSES.txt, copied into the project with them) made into the project's assets:
//   - kit_material: an ambientCG set as a material (Image Texture nodes: color, roughness, OpenGL normal through a Normal
//     Map), World Aligned (its tile in cm: walls and floors of any size tile at one size) or by the mesh's UV;
//   - kit_prop / kit_place: a Poly Haven model imported once (its parts: Static Meshes with their PBR materials), then
//     each placement a copy of its parts moved, turned and scaled together (one import however many are placed).
// Without DemoContent (an offline build) every call answers 0 / empty: the demos fall back to their colored shapes.

// Where the kit reads from and at what size: the build's DemoContent at 1K (the demos), or a scene's own sources (the
// haunted house: its 2K downloads next to the project, kit_source)
static const char* g_kit_root = NULL;  // NULL: DemoContent
static const char* g_kit_tres = "1K";  // ambientCG maps: <id>_<res>-JPG_<map>.jpg
static const char* g_kit_mres = "1k";  // Poly Haven models: <id>_<res>.gltf
static uint32_t g_kit_bake = 1024;     // the materials' bake size
static void kit_source(const char* root, bool hi) {
    g_kit_root = root, g_kit_tres = hi ? "2K" : "1K", g_kit_mres = hi ? "2k" : "1k", g_kit_bake = hi ? 2048u : 1024u;
}
static bool kit_dir(char* out, size_t cap) {
    char exe[700];
    if (g_kit_root) snprintf(out, cap, "%s", g_kit_root);
    else if (!re_exe_dir(exe, sizeof exe)) return false;
    else snprintf(out, cap, "%s/DemoContent", exe);
    return re_file_stat(out, NULL, NULL, NULL);
}

// The licenses go into the project with the assets (Content/Demo/LICENSES.txt)
static void kit_licenses(void) {
    char src[800], dst[800], dir[800];
    if (!kit_dir(src, sizeof src)) return;
    snprintf(dir, sizeof dir, "%s/Demo", content_root());
    re_make_dirs(dir);
    char from[900];
    snprintf(from, sizeof from, "%s/LICENSES.txt", src);
    snprintf(dst, sizeof dst, "%s/LICENSES.txt", dir);
    re_copy_file(from, dst);
}

static int kit_node(MatGraph* g, int type, float x, float y) { return mat_graph_add(g, type, x, y); }
static int kit_sock(const MatGraph* g, int node, bool out, const char* name) {
    const MatNodeDef* d = mat_node_def(g->nodes[node].type);
    int n = out ? d->nout : d->nin;
    for (int i = 0; i < n; i++)
        if (!strcmp(out ? d->out[i].name : d->in[i].name, name)) return i;
    return -1;
}
static void kit_link(MatGraph* g, int a, const char* ao, int b, const char* bi) { mat_graph_link(g, a, kit_sock(g, a, true, ao), b, kit_sock(g, b, false, bi)); }

// An ambientCG texture set (Textures/<id>/<id>_1K-JPG_<map>.jpg) as /Content/Demo/<demo>/Materials/M_<id>.rmat; tint
// multiplies the color (NULL: none); tile_cm > 0: World Aligned at that size, else the mesh's UV. Its hash (0: none).
static uint32_t kit_material(const char* demo, const char* id, float tile_cm, const float tint[3], float rough_scale) {
    char src[800], tdir[900], mdir[900], path[1000];
    if (!kit_dir(src, sizeof src)) return 0;
    snprintf(tdir, sizeof tdir, "%s/Demo/%s/Textures/%s", content_root(), demo, id);
    snprintf(mdir, sizeof mdir, "%s/Demo/%s/Materials", content_root(), demo);
    re_make_dirs(tdir), re_make_dirs(mdir);
    static const char* const maps[4] = { "Color", "Roughness", "NormalGL", "AmbientOcclusion" };
    bool have[4] = { 0 };
    for (int k = 0; k < 4; k++) {
        char from[1000], to[1000];
        snprintf(from, sizeof from, "%s/Textures/%s/%s_%s-JPG_%s.jpg", src, id, id, g_kit_tres, maps[k]);
        snprintf(to, sizeof to, "%s/%s_%s.jpg", tdir, id, maps[k]);
        have[k] = re_file_stat(to, NULL, NULL, NULL) || (re_file_stat(from, NULL, NULL, NULL) && re_copy_file(from, to));
    }
    if (!have[0]) return 0;
    snprintf(path, sizeof path, "%s/M_%s.rmat", mdir, id);
    Material* m = re_file_stat(path, NULL, NULL, NULL) ? mat_load(path) : mat_create(path, false, NULL);
    if (!m || !m->graph) return 0;
    MatGraph* g = m->graph;
    mat_graph_default(g);
    int bsdf = -1;
    for (int i = 0; i < g->node_count; i++)
        if (g->nodes[i].type == NODE_BSDF_PRINCIPLED) bsdf = i;
    char vp[400];
    int col = kit_node(g, NODE_TEX_IMAGE, -700, 0);
    snprintf(vp, sizeof vp, "/Content/Demo/%s/Textures/%s/%s_Color.jpg", demo, id, id);
    snprintf(g->nodes[col].image, MAT_PATH, "%s", vp);
    int base = col;
    const char* base_out = "Color";
    if (have[3]) { // the ambient occlusion darkens the color (its crevices)
        int ao = kit_node(g, NODE_TEX_IMAGE, -700, 520), mix = kit_node(g, NODE_MIX, -420, 60);
        snprintf(vp, sizeof vp, "/Content/Demo/%s/Textures/%s/%s_AmbientOcclusion.jpg", demo, id, id);
        snprintf(g->nodes[ao].image, MAT_PATH, "%s", vp);
        g->nodes[ao].prop[2] = IMAGE_COLORSPACE_NONCOLOR;
        g->nodes[mix].prop[1] = BLEND_MUL;
        g->nodes[mix].in[kit_sock(g, mix, false, "Factor")][0] = 1.0f;
        kit_link(g, col, "Color", mix, "A_Color");
        kit_link(g, ao, "Color", mix, "B_Color");
        base = mix, base_out = "Result_Color";
    }
    if (tint) {
        int rgb = kit_node(g, NODE_RGB, -420, -200), mul = kit_node(g, NODE_MIX, -220, 0);
        g->nodes[rgb].value[0] = tint[0], g->nodes[rgb].value[1] = tint[1], g->nodes[rgb].value[2] = tint[2], g->nodes[rgb].value[3] = 1.0f;
        g->nodes[mul].prop[1] = BLEND_MUL;
        g->nodes[mul].in[kit_sock(g, mul, false, "Factor")][0] = 1.0f;
        kit_link(g, base, base_out, mul, "A_Color");
        kit_link(g, rgb, "Color", mul, "B_Color");
        base = mul, base_out = "Result_Color";
    }
    kit_link(g, base, base_out, bsdf, "Base Color");
    if (have[1]) {
        int r = kit_node(g, NODE_TEX_IMAGE, -700, 180);
        snprintf(vp, sizeof vp, "/Content/Demo/%s/Textures/%s/%s_Roughness.jpg", demo, id, id);
        snprintf(g->nodes[r].image, MAT_PATH, "%s", vp);
        g->nodes[r].prop[2] = IMAGE_COLORSPACE_NONCOLOR;
        if (rough_scale > 0.0f && rough_scale != 1.0f) {
            int mm = kit_node(g, NODE_MATH, -420, 200);
            g->nodes[mm].prop[0] = MATH_MULTIPLY;
            g->nodes[mm].in[1][0] = rough_scale;
            kit_link(g, r, "Color", mm, "Value");
            kit_link(g, mm, "Value", bsdf, "Roughness");
        } else
            kit_link(g, r, "Color", bsdf, "Roughness");
    }
    if (have[2]) {
        int nt = kit_node(g, NODE_TEX_IMAGE, -700, 350), nm = kit_node(g, NODE_NORMAL_MAP, -420, 360);
        snprintf(vp, sizeof vp, "/Content/Demo/%s/Textures/%s/%s_NormalGL.jpg", demo, id, id);
        snprintf(g->nodes[nt].image, MAT_PATH, "%s", vp);
        g->nodes[nt].prop[2] = IMAGE_COLORSPACE_NONCOLOR;
        kit_link(g, nt, "Color", nm, "Color");
        kit_link(g, nm, "Normal", bsdf, "Normal");
    }
    m->resolution = g_kit_bake;
    m->world_aligned = tile_cm > 0.0f;
    m->world_tile = tile_cm > 0.0f ? tile_cm : 200.0f;
    mat_save(m);
    mat_mark_dirty(m);
    return m->hash;
}

// A Poly Haven model imported once: its parts (the actors the import placed) as the template the placements copy
#define KIT_PARTS 24
typedef struct KitProp { Entity parts[KIT_PARTS]; uint32_t n; float lift; } KitProp; // lift: its lowest point up to z 0

static KitProp kit_prop(Editor* ed, const char* demo, const char* id) {
    KitProp p;
    memset(&p, 0, sizeof p);
    char src[800], gltf[1000], folder[300];
    if (!kit_dir(src, sizeof src)) return p;
    snprintf(gltf, sizeof gltf, "%s/Models/%s/%s_%s.gltf", src, id, id, g_kit_mres);
    if (!re_file_stat(gltf, NULL, NULL, NULL)) return p;
    snprintf(folder, sizeof folder, "/Content/Demo/%s/Props/%s", demo, id);
    World* w = &ed->world;
    static uint8_t before[RE_ECS_MAX_ENTITIES / 8];
    memset(before, 0, (w->slot_count + 7) / 8);
    for (uint32_t s = 1; s < w->slot_count; s++)
        if (ecs_entity_at(w, s) != ENTITY_NONE) before[s >> 3] |= (uint8_t)(1u << (s & 7));
    if (!editor_import_tlend(ed, gltf, folder, true)) return p;
    float lo = 1e30f;
    for (uint32_t s = 1; s < w->slot_count && p.n < KIT_PARTS; s++) {
        Entity e = ecs_entity_at(w, s);
        if (e == ENTITY_NONE || (before[s >> 3] & (1u << (s & 7)))) continue;
        if (!(ecs_mask(w, e) & COMP_BIT(COMP_STATIC_MESH))) continue;
        p.parts[p.n++] = e;
        const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        uint32_t slot = render_mesh_slot(((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->mesh);
        float mn[3], mx[3];
        render_mesh_bounds(slot, mn, mx);
        if (t) lo = fminf(lo, t->location[2] + mn[2] * t->scale[2]);
    }
    p.lift = lo < 1e29f ? -lo : 0.0f;
    for (uint32_t i = 0; i < p.n; i++) { // the template out of sight until it is placed (kit_done removes it)
        CTransform* t = ecs_get(w, p.parts[i], COMP_TRANSFORM);
        if (t) t->flags |= TRANSFORM_HIDDEN | TRANSFORM_HIDDEN_GAME;
    }
    return p;
}

// A copy of the prop at (x, y, z) (its base on z), turned yaw degrees, scaled; collides (its bounds) when solid. Its first
// part (a name for the game mode to find it by; ENTITY_NONE without the prop)
static Entity kit_place(Editor* ed, const KitProp* p, float x, float y, float z, float yaw, float scale, bool solid) {
    World* w = &ed->world;
    Entity first = ENTITY_NONE;
    float c = cosf(yaw * 0.017453292519943295f), s = sinf(yaw * 0.017453292519943295f);
    for (uint32_t i = 0; i < p->n; i++) {
        Entity e = ecs_clone(w, p->parts[i]);
        CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (!t) continue;
        if (!first) first = e;
        float lx = t->location[0] * scale, ly = t->location[1] * scale, lz = (t->location[2] + p->lift) * scale;
        t->location[0] = x + lx * c - ly * s, t->location[1] = y + lx * s + ly * c, t->location[2] = z + lz;
        t->rotation[1] += yaw;
        for (int k = 0; k < 3; k++) t->scale[k] *= scale;
        t->flags = (t->flags & ~(TRANSFORM_HIDDEN | TRANSFORM_HIDDEN_GAME | TRANSFORM_MOBILITY_MASK)) | MOBILITY_STATIC;
        if (solid && i == 0) { // (a box of the first part's bounds, an actor of its own at their center: Characters walk around it)
            uint32_t slot = render_mesh_slot(((CStaticMesh*)ecs_get(w, e, COMP_STATIC_MESH))->mesh);
            float mn[3], mx[3], ctr[3];
            render_mesh_bounds(slot, mn, mx);
            for (int k = 0; k < 3; k++) ctr[k] = (mx[k] + mn[k]) * 0.5f * t->scale[k];
            Entity b = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_COLLISION));
            CTransform* bt = b ? ecs_get(w, b, COMP_TRANSFORM) : NULL;
            CCollision* co = b ? ecs_get(w, b, COMP_COLLISION) : NULL;
            if (bt && co) {
                bt->location[0] = t->location[0] + ctr[0] * c - ctr[1] * s, bt->location[1] = t->location[1] + ctr[0] * s + ctr[1] * c;
                bt->location[2] = t->location[2] + ctr[2];
                bt->rotation[1] = t->rotation[1], bt->scale[0] = bt->scale[1] = bt->scale[2] = 1.0f, bt->flags = MOBILITY_STATIC;
                co->shape = COLLISION_BOX, co->block = 1, co->generate_overlap_events = 0;
                for (int k = 0; k < 3; k++) co->extent[k] = (mx[k] - mn[k]) * 0.5f * t->scale[k];
            }
        }
        ecs_touch(w, e);
    }
    return first;
}

static void kit_done(Editor* ed, KitProp* p) {
    for (uint32_t i = 0; i < p->n; i++) ecs_destroy(&ed->world, p->parts[i]);
    p->n = 0;
}
