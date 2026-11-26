// (Q9) Soft bodies in the editor (softbody.h, cloth.h, notas-dev/soft-body.md): their meshes made when missing (a rounded
// cushion and cube, tessellated: a box of 24 vertices bends only at its corners), Quick Add > Soft Body, Actor > Make Soft
// Body from Selected (and the Content Browser's Create > Soft Body), the Details' section, the test scenes (sofa,
// softbody, softbody_rigid), their script and their checks. Included by editor.c.
static void label_cell(const char* label);
static bool row_float(const char* label, float* v, float speed, float mn, float mx, const char* fmt);
static bool row_check(const char* label, uint32_t* v);

// A rounded box (half extents h cm, its edges rounded by r cm), each face an n x n grid wound outward, written as
// /Content/Meshes/<name>.rmesh when missing (the meshes read again); its id
static uint32_t soft_rounded_box(Editor* ed, const char* name, const float h[3], float r, int n) {
    char vp[200], path[1100];
    snprintf(vp, sizeof vp, "/Content/Meshes/%s.rmesh", name);
    uint32_t id = render_mesh_id_of(vp);
    if (id && render_mesh_slot(id) >= SHAPE_COUNT) return id;
    snprintf(path, sizeof path, "%s/Meshes", content_root());
    re_make_dirs(path);
    snprintf(path, sizeof path, "%s/Meshes/%s.rmesh", content_root(), name);
    uint32_t nv = 6u * (uint32_t)(n + 1) * (uint32_t)(n + 1), ni = 6u * (uint32_t)n * (uint32_t)n * 6u;
    GpuVertex* v = calloc(nv, sizeof(GpuVertex));
    uint32_t* idx = malloc(sizeof(uint32_t) * ni);
    if (v && idx) {
        static const int fa[6][3] = { { 0, 1, 2 }, { 0, 2, 1 }, { 1, 2, 0 }, { 1, 0, 2 }, { 2, 0, 1 }, { 2, 1, 0 } }; // (axis, u, v: u x v outward)
        static const float fs[6] = { 1, -1, 1, -1, 1, -1 };
        uint32_t at = 0, ii = 0;
        for (int f = 0; f < 6; f++) {
            uint32_t base = at;
            for (int j = 0; j <= n; j++)
                for (int i = 0; i <= n; i++) {
                    float p[3], in[3], d[3];
                    p[fa[f][0]] = fs[f] * h[fa[f][0]];
                    p[fa[f][1]] = (-1.0f + 2.0f * (float)i / n) * h[fa[f][1]];
                    p[fa[f][2]] = (-1.0f + 2.0f * (float)j / n) * h[fa[f][2]];
                    float dl = 0.0f;
                    for (int k = 0; k < 3; k++) {
                        float lim = fmaxf(h[k] - r, 0.0f);
                        in[k] = fminf(fmaxf(p[k], -lim), lim), d[k] = p[k] - in[k], dl += d[k] * d[k];
                    }
                    dl = sqrtf(dl);
                    GpuVertex* g = &v[at++];
                    for (int k = 0; k < 3; k++) d[k] = dl > 1e-6f ? d[k] / dl : (k == fa[f][0] ? fs[f] : 0.0f), g->pos[k] = in[k] + r * d[k];
                    g->normal_oct = shapes_oct(d);
                    g->uv[0] = (float)i / n, g->uv[1] = 1.0f - (float)j / n, g->uv2[0] = g->uv2[1] = -1.0f;
                }
            for (int j = 0; j < n; j++)
                for (int i = 0; i < n; i++) {
                    uint32_t a = base + (uint32_t)(j * (n + 1) + i), b = a + 1, c = a + (uint32_t)(n + 1) + 1, d = a + (uint32_t)(n + 1);
                    idx[ii++] = a, idx[ii++] = b, idx[ii++] = c, idx[ii++] = a, idx[ii++] = c, idx[ii++] = d;
                }
        }
        MeshBuilt mb;
        if (mesh_build_indexed(v, nv, idx, ii, false, false, &mb)) {
            if (!rmesh_write(path, &mb)) printf("[RealEngine] FAIL: soft body: %s not written\n", name);
            mesh_built_free(&mb);
        }
    }
    free(v), free(idx);
    render_meshes_reload(ed->vk);
    return render_mesh_id_of(vp);
}
static uint32_t soft_cushion_mesh(Editor* ed) { return soft_rounded_box(ed, "SM_SoftCushion", (float[3]){ 31.0f, 30.0f, 8.0f }, 4.0f, 18); }
static uint32_t soft_cube_mesh(Editor* ed) { return soft_rounded_box(ed, "SM_SoftCube", (float[3]){ 20.0f, 20.0f, 20.0f }, 5.0f, 14); }

// A Cloth made a soft body of a preset, held as asked (Bottom: the band at its bottom, a cushion on its seat)
static void soft_set(CCloth* c, int preset, int attach) {
    ecs_component_defaults(COMP_CLOTH, c);
    c->kind = CLOTH_KIND_SOFT;
    soft_apply_preset(c, preset);
    c->attach = (uint32_t)attach, c->pin_axis[0] = c->pin_axis[1] = 0.0f, c->pin_axis[2] = 1.0f, c->pin_band = 1.0f;
    c->thickness = 0.4f, c->iterations = 6, c->self_collision = 1; // (6: the box sank the same with 4 or 8; each costs ~0.06 ms a body)
}

static Entity soft_actor(Editor* ed, const char* name, uint32_t mesh, const float loc[3], const float scale[3], const float col[3], int preset, int attach) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_CLOTH));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memset(t, 0, sizeof *t);
    memcpy(t->location, loc, 12), memcpy(t->scale, scale, 12), t->flags = MOBILITY_MOVABLE;
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    sm->mesh = mesh, sm->roughness = 0.8f, sm->cast_shadow = 1;
    memcpy(sm->base_color, col, 12);
    soft_set(ecs_get(w, e, COMP_CLOTH), preset, attach);
    return e;
}

// Quick Add > Soft Body > a preset: on the ground 3 m in front of the camera (a cushion, a cube of jelly, a rubber ball, a
// lump of flesh held at its middle)
static void add_soft(Editor* ed, int preset, double now) {
    editor_undo_push(ed);
    float cy = cosf(ed->cam.yaw * 0.017453293f), sy = sinf(ed->cam.yaw * 0.017453293f);
    float at[3] = { ed->cam.pos[0] + cy * 300.0f, ed->cam.pos[1] + sy * 300.0f, 0.0f };
    uint32_t mesh = preset == SOFT_JELLY ? soft_cube_mesh(ed) : preset == SOFT_RUBBER || preset == SOFT_FLESH ? (uint32_t)SHAPE_SPHERE : soft_cushion_mesh(ed);
    float sc[3] = { 1, 1, 1 };
    if (mesh == SHAPE_SPHERE) sc[0] = sc[1] = sc[2] = preset == SOFT_FLESH ? 0.4f : 0.5f;
    float mn[3], mx[3];
    render_mesh_bounds(mesh, mn, mx);
    at[2] = -mn[2] * sc[2] + (preset == SOFT_RUBBER || preset == SOFT_JELLY ? 80.0f : preset == SOFT_FLESH ? 100.0f : 0.0f); // (the ball and the jelly dropped)
    static const float cols[SOFT_PRESET_COUNT][3] = { { 0.6f, 0.6f, 0.6f }, { 0.75f, 0.72f, 0.62f }, { 0.7f, 0.08f, 0.06f }, { 0.15f, 0.6f, 0.12f },
                                                       { 0.18f, 0.24f, 0.42f }, { 0.75f, 0.5f, 0.42f } };
    int attach = preset == SOFT_CUSHION || preset == SOFT_FOAM ? SOFT_ATTACH_BOTTOM : preset == SOFT_FLESH ? SOFT_ATTACH_BONE : SOFT_ATTACH_FREE;
    char un[RE_NAME_MAX], nm[64];
    snprintf(nm, sizeof nm, "Soft%s", g_soft_preset_names[preset]);
    editor_unique_name(ed, nm, un, sizeof un);
    Entity e = soft_actor(ed, un, mesh, at, sc, cols[preset], preset, attach);
    if (e == ENTITY_NONE) return;
    editor_select(ed, e, SEL_REPLACE);
    ed->dirty = true;
    editor_status(ed, now, TR("Soft body placed: Play (Alt+P) to see it move; its preset, stiffness and what holds it in its Details (Soft Body)."));
}

// Actor > Make Soft Body from Selected: the selected Static Mesh actor (a cushion, a mattress, a ball; a part of a Skeletal
// Mesh: its flesh held to its bone) becomes a soft body of its own mesh, in place
static void make_soft_from_selected(Editor* ed) {
    World* w = &ed->world;
    Entity e = ed->selected;
    CStaticMesh* sm = e ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
    if (!sm || ecs_get(w, e, COMP_CLOTH) || ecs_get(w, e, COMP_RIGID_BODY)) {
        editor_status(ed, 0.0, "%s", TR("Select a Static Mesh actor (not a physics body) to make a soft body of it."));
        return;
    }
    editor_undo_push(ed);
    if (!ecs_add(w, e, COMP_CLOTH)) return;
    const CAttach* at = ecs_get(w, e, COMP_ATTACH);
    bool part = at && at->kind == ATTACH_SK_PART;
    CCloth* c = ecs_get(w, e, COMP_CLOTH);
    soft_set(c, part ? SOFT_FLESH : SOFT_CUSHION, part ? SOFT_ATTACH_BONE : SOFT_ATTACH_BOTTOM);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t) {
        t->flags = (t->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
        float ax[3][3];
        rotator_axes(t->rotation, ax); // (the world's up in the mesh's space: its bottom as it stands)
        for (int k = 0; k < 3; k++) c->pin_axis[k] = ax[k][2];
    }
    if (part) c->pin_band = 4.0f, c->attach_falloff = 10.0f; // (the part's origin is its joint: the flesh around it held, fading out)
    const CName* nm = ecs_get(w, e, COMP_NAME);
    editor_select(ed, e, SEL_REPLACE);
    ed->dirty = true;
    editor_status(ed, 0.0, TR("Soft body made from %s: Play (Alt+P) to see it; its stiffness and hold in Details > Soft Body."), nm ? nm->text : "");
}

// The Details' Soft Body section (a Cloth of kind Soft Body); true when a field changed
static bool soft_details(CCloth* c) {
    bool ch = false, custom = false;
    label_cell("Cloth Kind");
    if (igBeginCombo("##sbkind", (c->kind == CLOTH_KIND_SOFT ? TR("Soft Body") : TR("Sheet")), 0)) {
        if (igSelectable_Bool(TR("Sheet"), c->kind == CLOTH_KIND_SHEET, 0, (ImVec2){ 0, 0 })) c->kind = CLOTH_KIND_SHEET, c->reset = 1, ch = true;
        if (igSelectable_Bool(TR("Soft Body"), c->kind == CLOTH_KIND_SOFT, 0, (ImVec2){ 0, 0 })) c->kind = CLOTH_KIND_SOFT, c->reset = 1, ch = true;
        igEndCombo();
    }
    label_cell("Soft Body Preset");
    int pr = (int)c->soft_preset;
    if (igBeginCombo("##sbpre", TR(g_soft_preset_names[pr < SOFT_PRESET_COUNT ? pr : 0]), 0)) {
        for (int k = 0; k < SOFT_PRESET_COUNT; k++)
            if (igSelectable_Bool(TR(g_soft_preset_names[k]), k == pr, 0, (ImVec2){ 0, 0 })) {
                if (k) soft_apply_preset(c, k);
                else c->soft_preset = SOFT_CUSTOM;
                ch = true;
            }
        igEndCombo();
    }
    int res = (int)c->soft_res;
    label_cell("Lattice Cells");
    if (igSliderInt("##sbres", &res, 2, SOFT_MAX_RES, "%d", 0)) c->soft_res = (uint32_t)res, ch = true;
    custom |= row_float("Soft Density", &c->soft_density, 1.0f, 5.0f, 3000.0f, "%.0f kg/m3");
    custom |= row_float("Stiffness (Young)", &c->youngs, 0.1f, 0.2f, 5000.0f, "%.1f kPa");
    custom |= row_float("Keep Volume", &c->poisson, 0.005f, 0.0f, 0.49f, "%.3f");
    custom |= row_float("Soft Damping", &c->soft_damping, 0.001f, 0.0f, 1.0f, "%.3f");
    custom |= row_float("Plasticity", &c->plasticity, 0.01f, 0.0f, 1.0f, "%.2f");
    custom |= row_float("Dent Recovery", &c->recovery, 0.05f, 0.0f, 120.0f, c->recovery > 0.0f ? "%.1f s" : "never");
    custom |= row_float("Dent Yield", &c->yield, 0.01f, 0.0f, 1.0f, "%.2f");
    custom |= row_float("Friction", &c->friction, 0.01f, 0.0f, 1.0f, "%.2f");
    if (custom) c->soft_preset = SOFT_CUSTOM, ch = true;
    ch |= row_float("Thickness", &c->thickness, 0.05f, 0.05f, 20.0f, "%.2f cm");
    ch |= row_float("Gravity Scale", &c->gravity, 0.01f, -2.0f, 4.0f, "%.2f");
    label_cell("Held By");
    int at = (int)c->attach;
    if (igBeginCombo("##sbatt", TR(g_soft_attach_names[at < SOFT_ATTACH_COUNT ? at : 0]), 0)) {
        for (int k = 0; k < SOFT_ATTACH_COUNT; k++)
            if (igSelectable_Bool(TR(g_soft_attach_names[k]), k == at, 0, (ImVec2){ 0, 0 })) c->attach = (uint32_t)k, c->reset = 1, ch = true;
        igEndCombo();
    }
    if (c->attach != SOFT_ATTACH_FREE && c->attach != SOFT_ATTACH_PAINTED) {
        ch |= row_float("Hold Band", &c->pin_band, 0.1f, 0.0f, 500.0f, "%.1f cm");
        label_cell("Hold Axis");
        ch |= igDragFloat3("##sbax", c->pin_axis, 0.01f, -1.0f, 1.0f, "%.2f", 0);
    }
    if (c->attach != SOFT_ATTACH_FREE) {
        ch |= row_float("Hold Falloff", &c->attach_falloff, 0.1f, 0.0f, 500.0f, "%.1f cm");
        ch |= row_float("Hold Stiffness", &c->attach_stiffness, 10.0f, 0.0f, 1e6f, "%.0f N/m");
        bool pinned = !c->released;
        label_cell("Held");
        if (igCheckbox("##sbheld", &pinned)) c->released = pinned ? 0u : 1u, ch = true;
    }
    label_cell("Follow Actor");
    igSetNextItemWidth(-1);
    ch |= igInputText("##sbfa", c->follow_actor, sizeof c->follow_actor, 0, NULL, NULL);
    label_cell("Follow Bone");
    igSetNextItemWidth(-1);
    ch |= igInputText("##sbfb", c->follow_bone, sizeof c->follow_bone, 0, NULL, NULL);
    ch |= row_check("Collide With Others", &c->self_collision);
    if (c->self_collision) ch |= row_float("Contact Distance", &c->self_distance, 0.05f, 0.0f, 20.0f, c->self_distance > 0.0f ? "%.2f cm" : "auto");
    ch |= row_float("Push Physics Bodies", &c->rigid_push, 0.01f, 0.0f, 4.0f, "%.2fx");
    ch |= row_check("Sleep When Still", &c->sleep);
    if (c->sleep) ch |= row_float("Sleep Distance", &c->sleep_distance, 1.0f, 0.0f, 10000.0f, c->sleep_distance > 0.0f ? "%.0f m" : "never");
    int iters = (int)c->iterations, subs = (int)c->substeps;
    label_cell("Iterations");
    if (igSliderInt("##sbit", &iters, 1, 24, "%d", 0)) c->iterations = (uint32_t)iters, ch = true;
    label_cell("Substeps");
    if (igSliderInt("##sbsub", &subs, 1, 4, "%d", 0)) c->substeps = (uint32_t)subs, ch = true;
    label_cell("");
    if (igButton(TR("Reset Soft Body"), (ImVec2){ -1, 0 })) c->reset = 1;
    return ch;
}

// --- The test scenes ---
static Entity g_soft_jumper, g_soft_sitter, g_soft_box, g_soft_jelly, g_soft_ball, g_soft_cushions[3];
static double g_soft_t;
static float g_soft_box_z0, g_soft_box_range = 1e9f; // (the box's height range over the last whole second: it stopped)

static void soft_scene_begin(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    g_soft_jumper = g_soft_sitter = g_soft_box = g_soft_jelly = g_soft_ball = ENTITY_NONE;
    memset(g_soft_cushions, 0, sizeof g_soft_cushions);
    g_soft_t = 0.0;
    float g[3] = { 0.5f, 0.5f, 0.52f };
    Entity fl = mesh_actor(ed, SHAPE_PLANE, 0, 0, 0, g, 0, 0.8f);
    CTransform* ft = ecs_get(&ed->world, fl, COMP_TRANSFORM);
    ft->scale[0] = ft->scale[1] = 30.0f, ft->flags = MOBILITY_STATIC;
    ((CStaticMesh*)ecs_get(&ed->world, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
}
static void soft_scene_end(Editor* ed, float cx, float cy, float cz, float yaw, float pitch) {
    ed->cam.pos[0] = cx, ed->cam.pos[1] = cy, ed->cam.pos[2] = cz, ed->cam.yaw = yaw, ed->cam.pitch = pitch;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_DIRECTIONAL, (float[]){ 0, 0, 500 }, 0);
    CTransform* lt = ecs_get(&ed->world, ed->selected, COMP_TRANSFORM);
    lt->rotation[0] = -45.0f, lt->rotation[1] = 35.0f;
    spawn_actor(ed, SPAWN_LIGHT, LIGHT_SKY, (float[]){ 0, -300, 450 }, 0);
    ed->selected = ENTITY_NONE;
    ed->dirty = true;
}
static Entity soft_box(Editor* ed, float x, float y, float z, float sx, float sy, float sz, const float col[3]) {
    Entity e = mesh_actor(ed, SHAPE_CUBE, x, y, z, col, 0, 0.7f);
    CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    t->scale[0] = sx / 100.0f, t->scale[1] = sy / 100.0f, t->scale[2] = sz / 100.0f, t->flags = MOBILITY_STATIC;
    return e;
}

// --scene sofa: a sofa of three seat cushions (soft bodies held at their bottom); someone sits on the middle one, a
// Mannequin jumps on the right one (editor_soft_tick): the measure of the soft bodies' cost (notas-dev/soft-body.md)
static void sofa_scene(Editor* ed) {
    soft_scene_begin(ed);
    World* w = &ed->world;
    uint32_t cushion = soft_cushion_mesh(ed);
    const float wood[3] = { 0.25f, 0.15f, 0.08f }, fabric[3] = { 0.16f, 0.22f, 0.4f };
    soft_box(ed, 0, 0, 20, 200, 80, 40, wood);          // the base (its top at 40 cm)
    soft_box(ed, 0, 35, 60, 200, 14, 100, fabric);      // the back
    soft_box(ed, -100, 0, 50, 16, 80, 50, fabric);      // the arms
    soft_box(ed, 100, 0, 50, 16, 80, 50, fabric);
    for (int i = 0; i < 3; i++) {
        char nm[32];
        snprintf(nm, sizeof nm, "Cushion%d", i + 1);
        g_soft_cushions[i] = soft_actor(ed, nm, cushion, (float[3]){ -62.0f + 62.0f * (float)i, -5.0f, 48.0f }, (float[3]){ 1, 1, 1 }, fabric, SOFT_CUSHION, SOFT_ATTACH_BOTTOM);
    }
    // the one sitting: a seated body (a Character's capsule, its shape drawn by a cylinder) pressing the middle cushion
    Entity s = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_STATIC_MESH));
    snprintf(((CName*)ecs_get(w, s, COMP_NAME))->text, RE_NAME_MAX, "Sitter");
    CTransform* st = ecs_get(w, s, COMP_TRANSFORM);
    memset(st, 0, sizeof *st);
    st->scale[0] = st->scale[1] = 0.44f, st->scale[2] = 0.9f, st->flags = MOBILITY_MOVABLE;
    st->location[0] = 0.0f, st->location[1] = -10.0f, st->location[2] = 150.0f;
    CCharacter* sc = ecs_get(w, s, COMP_CHARACTER);
    sc->radius = 22.0f, sc->half_height = 45.0f, sc->gravity_scale = 0.0f, sc->on_ground = 1, sc->max_walk_speed = 0.0f, sc->auto_possess = 0;
    CStaticMesh* ssm = ecs_get(w, s, COMP_STATIC_MESH);
    ssm->mesh = SHAPE_CYLINDER, ssm->roughness = 0.6f, ssm->cast_shadow = 1;
    ssm->base_color[0] = 0.55f, ssm->base_color[1] = 0.35f, ssm->base_color[2] = 0.25f;
    g_soft_sitter = s;
    // the one jumping: the Mannequin over the right cushion, its legs' bones pressing it as it lands
    Entity j = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_CHARACTER) | COMP_BIT(COMP_SKELETAL_MESH));
    snprintf(((CName*)ecs_get(w, j, COMP_NAME))->text, RE_NAME_MAX, "Jumper");
    CTransform* jt = ecs_get(w, j, COMP_TRANSFORM);
    jt->scale[0] = jt->scale[1] = jt->scale[2] = 1.0f, jt->flags = MOBILITY_MOVABLE, jt->location[0] = 62.0f, jt->location[1] = -5.0f, jt->location[2] = 160.0f;
    CCharacter* jc = ecs_get(w, j, COMP_CHARACTER);
    jc->radius = 30.0f, jc->half_height = 90.0f, jc->gravity_scale = 0.0f, jc->on_ground = 1, jc->max_walk_speed = 0.0f, jc->auto_possess = 0;
    const SkelMesh* m = anim_mesh_name("SK_Mannequin");
    ((CSkeletalMesh*)ecs_get(w, j, COMP_SKELETAL_MESH))->mesh = m ? m->hash : 0;
    g_soft_jumper = j;
    soft_scene_end(ed, 30.0f, -330.0f, 150.0f, 95.0f, -12.0f);
}

// --scene softbody: a cube of jelly and a rubber ball dropped on the floor, the jelly squeezed down by an impulse: the
// volume test (softbody_volume: kept, never exploding)
static void softbody_scene(Editor* ed) {
    soft_scene_begin(ed);
    uint32_t cube = soft_cube_mesh(ed);
    g_soft_jelly = soft_actor(ed, "Jelly", cube, (float[3]){ -60.0f, 0.0f, 90.0f }, (float[3]){ 1, 1, 1 }, (float[3]){ 0.15f, 0.6f, 0.12f }, SOFT_JELLY, SOFT_ATTACH_FREE);
    g_soft_ball = soft_actor(ed, "RubberBall", SHAPE_SPHERE, (float[3]){ 60.0f, 0.0f, 120.0f }, (float[3]){ 0.5f, 0.5f, 0.5f }, (float[3]){ 0.7f, 0.08f, 0.06f }, SOFT_RUBBER,
                             SOFT_ATTACH_FREE);
    soft_scene_end(ed, 0.0f, -320.0f, 110.0f, 90.0f, -10.0f);
}

// --scene softbody_rigid: a 25 kg physics box dropped on a cushion lying on the floor: it sinks in and stops (the cushion's
// pushes back on it through physics_add_impulse_at)
static void softbody_rigid_scene(Editor* ed) {
    soft_scene_begin(ed);
    World* w = &ed->world;
    uint32_t cushion = soft_cushion_mesh(ed);
    g_soft_cushions[0] = soft_actor(ed, "Cushion", cushion, (float[3]){ 0.0f, 0.0f, 8.0f }, (float[3]){ 1, 1, 1 }, (float[3]){ 0.18f, 0.24f, 0.42f }, SOFT_CUSHION, SOFT_ATTACH_BOTTOM);
    Entity b = mesh_actor(ed, SHAPE_CUBE, 0.0f, 0.0f, 70.0f, (float[3]){ 0.7f, 0.5f, 0.2f }, 0, 0.6f);
    CTransform* bt = ecs_get(w, b, COMP_TRANSFORM);
    bt->scale[0] = bt->scale[1] = bt->scale[2] = 0.3f, bt->flags = MOBILITY_MOVABLE;
    snprintf(((CName*)ecs_get(w, b, COMP_NAME))->text, RE_NAME_MAX, "Box");
    physics_set_simulate(w, b, true);
    CRigidBody* rb = ecs_get(w, b, COMP_RIGID_BODY);
    if (rb) rb->mass = 25.0f, rb->enable_gravity = 1, rb->friction = 0.6f, rb->restitution = 0.0f, rb->linear_damping = 0.05f, rb->angular_damping = 0.2f;
    g_soft_box = b, g_soft_box_z0 = 70.0f;
    soft_scene_end(ed, 0.0f, -200.0f, 60.0f, 90.0f, -10.0f);
}

// The scenes' script (each frame, before the editor's): someone sitting down and staying, the Mannequin jumping, the jelly
// squeezed once
void editor_soft_tick(Editor* ed, double dt) {
    World* w = &ed->world;
    if (!ed->playing) return;
    g_soft_t += dt;
    float t = (float)g_soft_t;
    CTransform* st = g_soft_sitter ? ecs_get(w, g_soft_sitter, COMP_TRANSFORM) : NULL;
    if (st) { // down from above over 1.5 s, into the cushion's top (56 cm) by 9 cm, then settled with a little shift
        float s = fminf(t / 1.5f, 1.0f), ease = s * s * (3.0f - 2.0f * s);
        float bottom = 150.0f + (56.0f - 9.0f - 150.0f) * ease + (t > 1.5f ? 1.5f * sinf((t - 1.5f) * 1.3f) : 0.0f);
        st->location[0] = 0.0f, st->location[1] = -10.0f;
        st->location[2] = bottom + 45.0f; // (its capsule: 45 cm half height)
        CCharacter* c = ecs_get(w, g_soft_sitter, COMP_CHARACTER);
        if (c) c->on_ground = 1, c->velocity[0] = c->velocity[1] = c->velocity[2] = 0.0f;
    }
    CTransform* jt = g_soft_jumper ? ecs_get(w, g_soft_jumper, COMP_TRANSFORM) : NULL;
    if (jt) { // jumps of 0.8 s: up 35 cm, down into the cushion by 8 (the feet at 95 cm under the pelvis... its bones press it)
        float ph = fmodf(t, 0.8f) / 0.8f, arc = 4.0f * ph * (1.0f - ph);
        jt->location[0] = 62.0f, jt->location[1] = -5.0f; // (held over the right cushion: its capsule against the sofa's back pushed it off)
        jt->location[2] = 56.0f - 8.0f + 35.0f * arc + 90.0f; // (its feet 90 cm under its middle)
        CCharacter* c = ecs_get(w, g_soft_jumper, COMP_CHARACTER);
        if (c) c->on_ground = 1, c->velocity[0] = c->velocity[1] = c->velocity[2] = 0.0f;
    }
    if (g_soft_box) { // its height's range over each second (a second's window kept): still = it stopped
        static float zlo = 1e9f, zhi = -1e9f;
        static double w0;
        if (t < 0.05f) zlo = 1e9f, zhi = -1e9f, w0 = 0.0, g_soft_box_range = 1e9f;
        const CTransform* bt = ecs_get(w, g_soft_box, COMP_TRANSFORM);
        if (bt) zlo = fminf(zlo, bt->location[2]), zhi = fmaxf(zhi, bt->location[2]);
        if (g_soft_t - w0 >= 1.0) g_soft_box_range = zhi - zlo, zlo = 1e9f, zhi = -1e9f, w0 = g_soft_t;
    }
    if (g_soft_box && getenv("REALENGINE_SOFT_DEBUG")) { // (the box's way down, a line every few frames)
        static uint32_t fr;
        const CTransform* bt = ecs_get(w, g_soft_box, COMP_TRANSFORM);
        const CRigidBody* rb = ecs_get(w, g_soft_box, COMP_RIGID_BODY);
        const SoftStats* ss = soft_stats();
        if (bt && rb && (fr++ % 6u) == 0u)
            printf("[soft dbg] t %.2f box %.1f %.1f %.1f v %.1f %.1f %.1f sleep %u | pushes %u force %.1f N awake %u\n", t, bt->location[0], bt->location[1], bt->location[2],
                   rb->velocity[0], rb->velocity[1], rb->velocity[2], rb->sleeping, ss->rigid_pushes, ss->rigid_force, ss->awake);
    }
    static bool squeezed;
    if (g_soft_jelly && t < 0.1f) squeezed = false;
    if (g_soft_jelly && t > 2.0f && !squeezed) { // the jelly squeezed down hard (Add Soft Body Impulse)
        const CTransform* jj = ecs_get(w, g_soft_jelly, COMP_TRANSFORM);
        if (jj) cloth_soft_impulse(g_soft_jelly, (float[3]){ jj->location[0], jj->location[1], jj->location[2] + 20.0f }, 30.0f, (float[3]){ 0.0f, 0.0f, -300.0f });
        squeezed = true;
    }
}

// The lattice's volume now against its rest (the GPU's positions read back), its farthest vertex from its mean, NaNs
static bool soft_measure(Entity e, double* ratio, float* spread, float* zmin, uint32_t* bad) {
    static float buf[4 * 65536];
    const SoftTopo* tp = NULL;
    uint32_t n = cloth_soft_readback(e, buf, 65536, &tp);
    if (!n || !tp) return false;
    double vol = soft_volume(tp, buf);
    *ratio = tp->rest_volume > 0.0f ? vol / tp->rest_volume : 0.0;
    double m[3] = { 0, 0, 0 };
    uint32_t na = 0;
    *bad = 0, *zmin = 1e30f;
    for (uint32_t v = 0; v < n; v++) {
        if (!tp->cells[v]) continue;
        const float* p = &buf[4 * v];
        if (!(p[0] == p[0]) || !(p[1] == p[1]) || !(p[2] == p[2]) || fabsf(p[0]) > 1e4f || fabsf(p[2]) > 1e4f) {
            (*bad)++;
            continue;
        }
        for (int k = 0; k < 3; k++) m[k] += p[k];
        *zmin = fminf(*zmin, p[2] * 100.0f);
        na++;
    }
    *spread = 0.0f;
    for (int k = 0; k < 3 && na; k++) m[k] /= na;
    for (uint32_t v = 0; v < n && na; v++) {
        if (!tp->cells[v]) continue;
        const float* p = &buf[4 * v];
        float d = sqrtf((float)((p[0] - m[0]) * (p[0] - m[0]) + (p[1] - m[1]) * (p[1] - m[1]) + (p[2] - m[2]) * (p[2] - m[2]))) * 100.0f;
        if (d == d) *spread = fmaxf(*spread, d);
    }
    *spread /= fmaxf(tp->radius, 1.0f);
    return true;
}

// The scenes' checks at the measure's end (main.c): false on a FAIL line
bool editor_soft_check(Editor* ed, const char* scene) {
    World* w = &ed->world;
    const SoftStats* ss = soft_stats();
    printf("[RealEngine] Soft bodies: %u (%u awake), %u lattice vertices, %u tetrahedra, %u physics bodies pushed (max %.1f N), lattice built in %.1f ms\n",
           ss->bodies, ss->awake, ss->vertices, ss->tets, ss->rigid_pushes, ss->rigid_force, ss->build_ms);
    bool ok = ss->bodies > 0;
    if (!ok) printf("[RealEngine] FAIL: soft body: none simulated\n");
    if (!strcmp(scene, "softbody")) {
        Entity es[2] = { g_soft_jelly, g_soft_ball };
        const char* nm[2] = { "jelly", "rubber ball" };
        for (int i = 0; i < 2; i++) {
            double r;
            float spread, zmin;
            uint32_t bad;
            if (!soft_measure(es[i], &r, &spread, &zmin, &bad)) {
                printf("[RealEngine] FAIL: soft body: the %s not read back\n", nm[i]);
                ok = false;
                continue;
            }
            printf("[RealEngine] Soft body %s: volume %.1f%% of its rest, spread %.2fx its radius, lowest %.1f cm, %u bad vertices\n", nm[i], r * 100.0, spread, zmin, bad);
            if (bad) printf("[RealEngine] FAIL: soft body: the %s has NaN or flung vertices\n", nm[i]), ok = false;
            if (r < 0.85 || r > 1.15) printf("[RealEngine] FAIL: soft body: the %s's volume not kept (%.1f%%)\n", nm[i], r * 100.0), ok = false;
            if (spread > 1.6f) printf("[RealEngine] FAIL: soft body: the %s exploded (spread %.2f)\n", nm[i], spread), ok = false;
            // (the jelly rests on the floor; the rubber ball may still be bouncing: it fell from 95 cm, it never went under)
            if ((i == 0 ? zmin > 12.0f : zmin > 85.0f) || zmin < -3.0f) printf("[RealEngine] FAIL: soft body: the %s isn't on the floor (lowest %.1f cm)\n", nm[i], zmin), ok = false;
        }
    }
    if (!strcmp(scene, "softbody_rigid")) {
        const CTransform* bt = g_soft_box ? ecs_get(w, g_soft_box, COMP_TRANSFORM) : NULL;
        const CRigidBody* rb = g_soft_box ? ecs_get(w, g_soft_box, COMP_RIGID_BODY) : NULL;
        if (!bt || !rb) return printf("[RealEngine] FAIL: soft body rigid: no box\n"), false;
        float sink = (16.0f + 15.0f) - bt->location[2]; // (the cushion's top at 16 cm, the box's half 15)
        float sp = sqrtf(rb->velocity[0] * rb->velocity[0] + rb->velocity[1] * rb->velocity[1] + rb->velocity[2] * rb->velocity[2]);
        printf("[RealEngine] Soft body rigid: the box (25 kg) sank %.1f cm into the cushion, its center at %.1f cm, moving %.2f cm in the last second "
               "(%.1f cm/s now)\n", sink, bt->location[2], g_soft_box_range, sp);
        if (sink < 0.5f) printf("[RealEngine] FAIL: soft body rigid: the box didn't sink in (%.1f cm)\n", sink), ok = false;
        if (sink > 13.0f) printf("[RealEngine] FAIL: soft body rigid: the box went through the cushion (%.1f cm)\n", sink), ok = false;
        // (stopped: under 1.5 cm of travel in a whole second; the pushes read back 2-3 frames late leave a tremor of a few mm)
        if (g_soft_box_range > 1.5f) printf("[RealEngine] FAIL: soft body rigid: the box didn't stop (%.2f cm in the last second)\n", g_soft_box_range), ok = false;
    }
    if (!strcmp(scene, "sofa")) {
        double r;
        float spread, zmin;
        uint32_t bad;
        if (soft_measure(g_soft_cushions[1], &r, &spread, &zmin, &bad))
            printf("[RealEngine] Soft body sofa: the middle cushion at %.1f%% of its volume, %u bad vertices\n", r * 100.0, bad);
        else
            printf("[RealEngine] FAIL: soft body sofa: the middle cushion not read back\n"), ok = false;
        // its GPU time (--gpu-passes): the simulation's pass and the skins' (the cushions' meshes bent)
        const GpuPassStat* ps;
        uint32_t nf = 0, np = vkctx_pass_stats(&ps, &nf);
        double sim = -1.0, skin = 0.0;
        for (uint32_t k = 0; k < np && nf; k++) {
            if (!strcmp(ps[k].name, "tecido (simulacao)")) sim = ps[k].ms / nf;
            if (!strcmp(ps[k].name, "skin (ossos e BLAS)")) skin = ps[k].ms / nf;
        }
        if (sim >= 0.0) {
            printf("[RealEngine] Soft body sofa: GPU %.3f ms the simulation + %.3f ms the skins (meshes and their BLAS, the Mannequin's too)\n", sim, skin);
            if (sim > 0.5) printf("[RealEngine] FAIL: soft body sofa: the simulation costs %.3f ms (the goal: under 0.5)\n", sim), ok = false;
        }
    }
    return ok;
}
