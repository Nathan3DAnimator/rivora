// (Q14) Splines and cables in the editor (spline.h, notas-dev/splines.md): Place Actors > World > Spline, Spline Mesh (a
// road), Spline Instances (a fence) and Cable; the Details' Spline / Spline Mesh / Instances / Cable sections; the example
// meshes made when missing (SM_SplineRoad, SM_SplinePipe: rings along X, so a piece bends smoothly); the test scene
// `splines`, its script and its check. Included by editor.c.
static void label_cell(const char* label);
static void label_cell_text(const char* shown); // (editor.c: a label already translated)
static bool row_float(const char* label, float* v, float speed, float mn, float mx, const char* fmt);
static bool row_check(const char* label, uint32_t* v);
static bool row_color(const char* label, float c[3], bool hdr);
static bool row_slider(const char* label, float* v, float mn, float mx);
static bool begin_section(const char* title, const char* id);
static bool field_static_mesh(const char* id, uint32_t* mesh);
static bool field_material(const char* id, uint32_t* hash, const char* none_label, bool lf_only);

enum { SPLINE_PLACE_SPLINE = 0, SPLINE_PLACE_MESH, SPLINE_PLACE_INST, SPLINE_PLACE_CABLE, SPLINE_PLACE_COUNT };
static const char* const g_spline_place_names[SPLINE_PLACE_COUNT] = { "Spline", "Spline Mesh", "Spline Instances", "Cable" };

// A mesh along X made of rings (n_len rings from 0 to len cm; the profile's points across, y z), written as
// /Content/Meshes/<name>.rmesh when missing; its id
static uint32_t spline_profile_mesh(Editor* ed, const char* name, const float (*prof)[2], int np, bool closed, float len, int n_len) {
    char vp[200], path[1100];
    snprintf(vp, sizeof vp, "/Content/Meshes/%s.rmesh", name);
    uint32_t id = render_mesh_id_of(vp);
    if (id && render_mesh_slot(id) >= SHAPE_COUNT) return id;
    snprintf(path, sizeof path, "%s/Meshes", content_root());
    re_make_dirs(path);
    snprintf(path, sizeof path, "%s/Meshes/%s.rmesh", content_root(), name);
    uint32_t nv = (uint32_t)(np * 2) * (uint32_t)(n_len + 1), ni = (uint32_t)(np * 2) * (uint32_t)n_len * 6u;
    GpuVertex* v = calloc(nv, sizeof(GpuVertex));
    uint32_t* idx = malloc(sizeof(uint32_t) * ni);
    if (v && idx) {
        // each edge of the profile its own two columns of vertices (hard edges: a road's curb stays sharp)
        uint32_t at = 0, ii = 0, edges = (uint32_t)(closed ? np : np - 1);
        float u_acc = 0.0f;
        for (uint32_t e = 0; e < edges; e++) {
            const float* a = prof[e];
            const float* b = prof[(e + 1) % (uint32_t)np];
            float dy = b[0] - a[0], dz = b[1] - a[1], el = sqrtf(dy * dy + dz * dz);
            float n[3] = { 0.0f, -dz / fmaxf(el, 1e-6f), dy / fmaxf(el, 1e-6f) }; // (the profile clockwise seen down +X: out on its left)
            uint32_t base = at;
            for (int r = 0; r <= n_len; r++)
                for (int k = 0; k < 2; k++) {
                    const float* p = k ? b : a;
                    GpuVertex* g = &v[at++];
                    g->pos[0] = len * (float)r / (float)n_len, g->pos[1] = p[0], g->pos[2] = p[1];
                    g->normal_oct = shapes_oct(n);
                    g->uv[0] = (u_acc + (k ? el : 0.0f)) / 100.0f, g->uv[1] = len * (float)r / (float)n_len / 100.0f; // (a meter a tile)
                    g->uv2[0] = g->uv2[1] = -1.0f;
                }
            u_acc += el;
            for (int r = 0; r < n_len; r++) {
                uint32_t q = base + (uint32_t)r * 2u;
                idx[ii++] = q, idx[ii++] = q + 2, idx[ii++] = q + 1, idx[ii++] = q + 1, idx[ii++] = q + 2, idx[ii++] = q + 3;
            }
        }
        MeshBuilt mb;
        if (mesh_build_indexed(v, at, idx, ii, false, false, &mb)) {
            if (!rmesh_write(path, &mb)) printf("[RealEngine] FAIL: spline mesh: %s not written\n", name);
            mesh_built_free(&mb);
        }
    }
    free(v), free(idx);
    render_meshes_reload(ed->vk);
    return render_mesh_id_of(vp);
}
// A road 4 m wide with 15 cm curbs and a 2 m run (16 rings: it bends smoothly over a long piece)
static uint32_t spline_road_mesh(Editor* ed) {
    static const float p[8][2] = { { -230, -40 }, { -230, 15 }, { -200, 15 }, { -200, 0 }, { 200, 0 }, { 200, 15 }, { 230, 15 }, { 230, -40 } }; // (clockwise seen down +X, as the pipe)
    return spline_profile_mesh(ed, "SM_SplineRoad", p, 8, false, 200.0f, 16);
}
// A pipe of 20 cm radius, 16 sides, 2 m long (16 rings)
static uint32_t spline_pipe_mesh(Editor* ed) {
    float p[16][2];
    for (int i = 0; i < 16; i++) { // (clockwise seen down +X: the triangles' winding faces out)
        float a = -6.2831853f * (float)i / 16.0f;
        p[i][0] = 20.0f * cosf(a), p[i][1] = 20.0f * sinf(a);
    }
    return spline_profile_mesh(ed, "SM_SplinePipe", (const float(*)[2])p, 16, true, 200.0f, 16);
}

static Entity spline_actor(World* w, const char* name, uint32_t kind, const float at[3]) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SPLINE));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memset(t, 0, sizeof *t);
    memcpy(t->location, at, 12);
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f, t->flags = MOBILITY_STATIC;
    spline_component_defaults(ecs_get(w, e, COMP_SPLINE), kind);
    return e;
}

// Place Actors > World > Spline / Spline Mesh / Spline Instances / Cable: 6 m in front of the camera, on the ground
static void add_spline(Editor* ed, int what, double now) {
    float cy = cosf(ed->cam.yaw * 0.017453293f), sy = sinf(ed->cam.yaw * 0.017453293f);
    float at[3] = { ed->cam.pos[0] + cy * 600.0f, ed->cam.pos[1] + sy * 600.0f, 0.0f };
    float gz;
    if (landscape_height(at[0], at[1], &gz)) at[2] = gz;
    uint32_t road = what == SPLINE_PLACE_MESH ? spline_road_mesh(ed) : 0; // (before the undo point: the meshes reload)
    editor_undo_push(ed);
    World* w = &ed->world;
    Entity e = spline_actor(w, g_spline_place_names[what], what == SPLINE_PLACE_CABLE ? SPLINE_KIND_CABLE : SPLINE_KIND_SPLINE, at);
    if (e == ENTITY_NONE) return;
    CRail* r = ecs_get(w, e, COMP_SPLINE);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->rotation[1] = ed->cam.yaw - 90.0f; // (across the view: the whole curve in sight)
    if (what != SPLINE_PLACE_CABLE) { // a gentle S of three points
        r->npoints = 3;
        const float pts[3][3] = { { -600, 0, 0 }, { 0, 250, 0 }, { 600, 0, 0 } };
        memcpy(r->points, pts, sizeof pts);
    }
    if (what == SPLINE_PLACE_MESH) r->mesh_on = 1, r->mesh = road ? road : SHAPE_CUBE, r->mesh_axis = 0, r->mesh_material = content_material("M_Grid.rmat");
    if (what == SPLINE_PLACE_INST) { // (fence posts: 12 cm thick, 1.2 m tall, standing on the curve)
        r->inst_on = 1, r->inst_mesh = SHAPE_CYLINDER, r->inst_spacing = 150.0f, r->inst_scale_min = 0.9f, r->inst_scale_max = 1.1f;
        r->inst_scale3[0] = r->inst_scale3[1] = 0.12f, r->inst_scale3[2] = 1.2f, r->inst_up = 60.0f;
    }
    if (what == SPLINE_PLACE_CABLE) t->location[2] += 300.0f, t->flags = MOBILITY_MOVABLE;
    char un[RE_NAME_MAX];
    editor_unique_name(ed, g_spline_place_names[what], un, sizeof un);
    set_name(w, e, un);
    editor_select(ed, e, SEL_REPLACE);
    ed->dirty = true;
    editor_status(ed, now, what == SPLINE_PLACE_CABLE ? TR("Cable placed: it hangs from this actor to its End Offset; give its ends actors (and sockets) in its Details.")
                                                      : TR("Spline placed: drag its points in the viewport (Alt: a new one; right button: add / delete); its meshes in its Details."));
}

// An actor picked from the level's (by name: what the cable stores); "" this one
static bool actor_combo(World* w, const char* id, char name[32], Entity self) {
    bool ch = false;
    igSetNextItemWidth(-1);
    if (igBeginCombo(id, name[0] ? name : TR("(this actor)"), 0)) {
        if (igSelectable_Bool(TR("(this actor)"), !name[0], 0, (ImVec2){ 0, 0 })) name[0] = 0, ch = true;
        uint32_t shown = 0;
        for (uint32_t ai = 0; ai < w->archetype_count && shown < 400; ai++) {
            const EcsArchetype* ar = &w->archetypes[ai];
            if (!(ar->mask & COMP_BIT(COMP_NAME)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || (ar->mask & COMP_BIT(COMP_ATTACH)) || !ar->count) continue;
            const CName* nm = ecs_arch_column(ar, COMP_NAME);
            for (uint32_t k = 0; k < ar->count && shown < 400; k++) {
                if (ecs_arch_entities(ar)[k] == self || !nm[k].text[0]) continue;
                shown++;
                if (igSelectable_Bool(nm[k].text, !strcmp(nm[k].text, name), 0, (ImVec2){ 0, 0 })) snprintf(name, 32, "%s", nm[k].text), ch = true;
            }
        }
        igEndCombo();
    }
    return ch;
}

static bool vec3_cell(const char* label, float v[3], float speed, const char* fmt) {
    label_cell(label);
    igPushID_Str(label);
    bool c = igDragFloat3("##v3", v, speed, -1e7f, 1e7f, fmt, 0);
    igPopID();
    return c;
}
static bool combo_cell(const char* label, uint32_t* v, const char* const* names, int n) {
    label_cell(label);
    const char* tn[8];
    for (int i = 0; i < n && i < 8; i++) tn[i] = TR(names[i]);
    int x = *v < (uint32_t)n ? (int)*v : 0;
    igPushID_Str(label);
    bool c = igCombo_Str_arr("##cb", &x, tn, n, -1);
    igPopID();
    if (c) *v = (uint32_t)x;
    return c;
}
static bool uint_cell(const char* label, uint32_t* v, uint32_t mn, uint32_t mx) {
    float f = (float)*v;
    if (!row_float(label, &f, 0.2f, (float)mn, (float)mx, "%.0f")) return false;
    *v = (uint32_t)fminf(fmaxf(f, (float)mn), (float)mx);
    return true;
}

// The Details' sections of a Spline / Rail / Cable (their tables opened and closed here); true: something changed
static bool spline_details(Editor* ed, World* w, Entity e, CRail* rl) {
    bool ch = false;
    static int s_point; // the point whose type, roll, scale and tangents are shown
    uint32_t kind = spline_kind(rl);
    const char* title = kind == SPLINE_KIND_CABLE ? "Cable" : kind == SPLINE_KIND_RAIL ? "Rail" : "Spline";
    if (begin_section(title, "sec_rail")) {
        if (!ed->playing) { // (the kind: a Rail becomes a Spline with meshes, or back)
            uint32_t k = kind;
            if (combo_cell("Spline Kind", &k, g_spline_kind_names, SPLINE_KIND_COUNT) && k != kind) {
                rl->kind = k, ch = true;
                if (k == SPLINE_KIND_CABLE && rl->cable_segments == 0) {
                    CRail d;
                    spline_component_defaults(&d, SPLINE_KIND_CABLE);
                    memcpy(&rl->cable_start_actor, &d.cable_start_actor, sizeof *rl - offsetof(CRail, cable_start_actor));
                }
            }
        }
        if (kind != SPLINE_KIND_CABLE) {
            uint32_t closed = rl->closed;
            if (row_check("Closed Loop", &closed)) rl->closed = closed, ch = true;
            if (kind == SPLINE_KIND_RAIL) ch |= row_float("Gauge", &rl->gauge, 0.5f, 30.0f, 400.0f, "%.1f cm");
            SplineCurve* c = RE_FRAME_ALLOC(SplineCurve, 1);
            if (c) {
                spline_curve_from(c, rl, NULL);
                label_cell("Spline Length");
                igText("%.1f m  (%u %s)", c->length / 100.0f, rl->npoints, TR("points"));
            }
            for (uint32_t i = 0; i < rl->npoints; i++) {
                char lb[32];
                snprintf(lb, sizeof lb, "%s %u", TR("Point"), i + 1);
                label_cell_text(lb);
                igPushID_Int((int)i + 7000);
                igSetNextItemWidth(-1);
                ch |= igDragFloat3("##pt", rl->points[i], 10.0f, -10000000.0f, 10000000.0f, "%.0f", 0);
                if (igIsItemActivated()) s_point = (int)i;
                igPopID();
            }
            label_cell("");
            if (rl->npoints < RAIL_MAX_POINTS && igButton(TR("+ Point"), (ImVec2){ 0, 0 })) spline_insert_point(rl, (int)rl->npoints - 1), ch = true;
            igSameLine(0, 6);
            if (rl->npoints > 2 && igButton(TR("- Point"), (ImVec2){ 0, 0 })) spline_remove_point(rl, (int)rl->npoints - 1), ch = true;
            if (rl->npoints) { // the chosen point's details
                if (s_point >= (int)rl->npoints) s_point = (int)rl->npoints - 1;
                label_cell("Edit Point");
                int sp = s_point + 1;
                if (igSliderInt("##editpt", &sp, 1, (int)rl->npoints, "%d", ImGuiSliderFlags_AlwaysClamp)) s_point = sp - 1;
                uint32_t ty = rl->ptype[s_point];
                if (combo_cell("Point Type", &ty, g_spline_point_names, SPLINE_POINT_COUNT)) spline_set_point_type(rl, s_point, ty), ch = true;
                if (rl->ptype[s_point] == SPLINE_POINT_CUSTOM) {
                    ch |= vec3_cell("Arrive Tangent", rl->arrive[s_point], 5.0f, "%.0f");
                    ch |= vec3_cell("Leave Tangent", rl->leave[s_point], 5.0f, "%.0f");
                }
                ch |= row_float("Point Roll", &rl->roll[s_point], 0.5f, -3600.0f, 3600.0f, "%.1f\xc2\xb0");
                label_cell("Point Scale");
                igPushID_Str("pscale");
                ch |= igDragFloat2("##ps", rl->pscale[s_point], 0.01f, -100.0f, 100.0f, "%.2f", 0);
                igPopID();
            }
        }
        if (kind == SPLINE_KIND_CABLE) { // Unreal's Cable Component
            label_cell("Start Actor");
            ch |= actor_combo(w, "##cstart", rl->cable_start_actor, e);
            label_cell("Start Socket");
            ch |= igInputText("##csock", rl->cable_start_socket, sizeof rl->cable_start_socket, 0, NULL, NULL);
            ch |= vec3_cell("Start Offset", rl->cable_start_offset, 1.0f, "%.0f");
            ch |= row_check("Free End", &rl->cable_end_free);
            label_cell("End Actor");
            ch |= actor_combo(w, "##cend", rl->cable_end_actor, e);
            label_cell("End Socket");
            ch |= igInputText("##esock", rl->cable_end_socket, sizeof rl->cable_end_socket, 0, NULL, NULL);
            ch |= vec3_cell("End Offset", rl->cable_end_offset, 1.0f, "%.0f");
            ch |= row_float("Cable Length", &rl->cable_length, 1.0f, 0.0f, 1e6f, rl->cable_length > 0.0f ? "%.0f cm" : "auto");
            label_cell("");
            if (igButton(TR("Fit to the Ends (+10%)"), (ImVec2){ -1, 0 })) { // (its ends' distance now, a little slack)
                float pts[CABLE_MAX_SEGMENTS + 1][3];
                uint32_t n = cable_points(w, e, pts, CABLE_MAX_SEGMENTS + 1);
                if (n >= 2) {
                    float d[3] = { pts[n - 1][0] - pts[0][0], pts[n - 1][1] - pts[0][1], pts[n - 1][2] - pts[0][2] };
                    rl->cable_length = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) * 1.1f, ch = true;
                }
            }
            ch |= uint_cell("Num Segments", &rl->cable_segments, 1, CABLE_MAX_SEGMENTS);
            ch |= row_slider("Stiffness", &rl->cable_stiffness, 0.0f, 1.0f);
            ch |= row_slider("Bend Stiffness", &rl->cable_bend, 0.0f, 1.0f);
            ch |= uint_cell("Solver Iterations", &rl->cable_iterations, 1, 32);
            ch |= uint_cell("Substeps", &rl->cable_substeps, 1, 16);
            ch |= row_slider("Damping", &rl->cable_damping, 0.0f, 5.0f);
            ch |= row_float("Gravity Scale", &rl->cable_gravity, 0.01f, -4.0f, 4.0f, "%.2f");
            ch |= row_float("Cable Width", &rl->cable_width, 0.1f, 0.1f, 100.0f, "%.1f cm");
            static const char* const draws[CABLE_DRAW_COUNT] = { "Tube", "Ribbon", "Hidden" };
            ch |= combo_cell("Draw As", &rl->cable_render, draws, CABLE_DRAW_COUNT);
            uint32_t cw = (rl->cable_collide & SPLINE_COLLIDE_WORLD) != 0, cb = (rl->cable_collide & SPLINE_COLLIDE_BODIES) != 0;
            if (row_check("Collide with World", &cw)) rl->cable_collide = (rl->cable_collide & ~SPLINE_COLLIDE_WORLD) | (cw ? SPLINE_COLLIDE_WORLD : 0u), ch = true;
            if (row_check("Collide with Bodies", &cb)) rl->cable_collide = (rl->cable_collide & ~SPLINE_COLLIDE_BODIES) | (cb ? SPLINE_COLLIDE_BODIES : 0u), ch = true;
            ch |= row_slider("Collision Friction", &rl->cable_friction, 0.0f, 1.0f);
            ch |= row_float("Wind Speed", &rl->cable_wind, 0.05f, 0.0f, 60.0f, "%.1f m/s");
            ch |= row_float("Wind Direction", &rl->cable_wind_dir, 1.0f, -360.0f, 360.0f, "%.0f\xc2\xb0");
            ch |= row_slider("Wind Gusts", &rl->cable_gusts, 0.0f, 1.0f);
            label_cell("Cable Material");
            ch |= field_material("##cmat", &rl->cable_material, TR("None (the color below)"), false);
            ch |= row_color("Cable Color", rl->cable_color, false);
            ch |= row_slider("Cable Roughness", &rl->cable_roughness, 0.0f, 1.0f);
            const SplineStats* st = spline_stats();
            label_cell("Simulation");
            igTextDisabled("%u %s, %u %s, %.3f ms", st->particles, TR("particles"), st->contacts, TR("contacts"), st->cable_ms);
        }
        igEndTable();
    }
    if (kind == SPLINE_KIND_SPLINE && begin_section("Spline Mesh", "sec_spline_mesh")) { // Unreal's Spline Mesh Component along it
        ch |= row_check("Spline Mesh", &rl->mesh_on);
        if (rl->mesh_on) {
            label_cell("Mesh");
            ch |= field_static_mesh("##smesh", &rl->mesh);
            static const char* const axes[3] = { "X", "Y", "Z" };
            ch |= combo_cell("Forward Axis", &rl->mesh_axis, axes, 3);
            ch |= row_float("Mesh Length", &rl->mesh_length, 1.0f, 0.0f, 1e6f, rl->mesh_length > 1.0f ? "%.0f cm" : "a segment");
            label_cell("Material");
            ch |= field_material("##smat", &rl->mesh_material, TR("None (the color below)"), false);
            ch |= row_color("Base Color", rl->mesh_color, false);
            ch |= row_slider("Roughness", &rl->mesh_roughness, 0.0f, 1.0f);
            ch |= row_slider("Metallic", &rl->mesh_metallic, 0.0f, 1.0f);
            label_cell("");
            if (igButton(TR("Road (example mesh)"), (ImVec2){ 0, 0 })) rl->mesh = spline_road_mesh(ed), rl->mesh_axis = 0, ch = true;
            igSameLine(0, 6);
            if (igButton(TR("Pipe (example mesh)"), (ImVec2){ 0, 0 })) rl->mesh = spline_pipe_mesh(ed), rl->mesh_axis = 0, ch = true;
            rl = ecs_get(w, e, COMP_SPLINE); // (the meshes reloaded: the world kept, the row too)
        }
        igEndTable();
    }
    if (kind == SPLINE_KIND_SPLINE && rl && begin_section("Road", "sec_spline_road")) { // (Q27.2) the traffic's cars drive it
        uint32_t on = rl->road_lanes > 0;
        if (row_check("Traffic Road", &on)) {
            rl->road_lanes = on ? 1u : 0u, ch = true;
            if (on && rl->road_speed <= 0.0f) rl->road_speed = 50.0f, rl->road_density = 4.0f, rl->road_lane_width = 350.0f;
        }
        if (rl->road_lanes) {
            ch |= uint_cell("Lanes Each Way", &rl->road_lanes, 1, 4);
            ch |= row_check("One Way", &rl->road_oneway);
            ch |= row_float("Speed Limit", &rl->road_speed, 1.0f, 5.0f, 300.0f, "%.0f km/h");
            ch |= row_float("Traffic Density", &rl->road_density, 0.1f, 0.0f, 20.0f, "%.1f / 100 m");
            ch |= row_float("Lane Width", &rl->road_lane_width, 5.0f, 200.0f, 600.0f, "%.0f cm");
            ch |= row_float("Signal Green Time", &rl->road_signal, 0.5f, 0.0f, 120.0f, rl->road_signal > 0.0f ? "%.0f s" : "none");
        }
        igEndTable();
    }
    if (kind == SPLINE_KIND_SPLINE && rl && begin_section("Instances Along Spline", "sec_spline_inst")) {
        ch |= row_check("Instances", &rl->inst_on);
        if (rl->inst_on) {
            label_cell("Instance Mesh");
            ch |= field_static_mesh("##imesh", &rl->inst_mesh);
            label_cell("Instance Material");
            ch |= field_material("##imat", &rl->inst_material, TR("None (the color below)"), false);
            ch |= row_color("Instance Color", rl->inst_color, false);
            ch |= row_float("Spacing", &rl->inst_spacing, 1.0f, 1.0f, 1e6f, "%.0f cm");
            ch |= row_float("Start Offset", &rl->inst_start, 1.0f, 0.0f, 1e6f, "%.0f cm");
            ch |= uint_cell("Random Seed", &rl->inst_seed, 0, 1000000);
            ch |= row_float("Side Offset", &rl->inst_side, 1.0f, -1e5f, 1e5f, "%.0f cm");
            ch |= row_float("Random Side Offset", &rl->inst_jitter, 1.0f, 0.0f, 1e5f, "%.0f cm");
            ch |= row_float("Random Yaw", &rl->inst_yaw_jitter, 0.5f, 0.0f, 180.0f, "%.0f\xc2\xb0");
            ch |= vec3_cell("Instance Scale", rl->inst_scale3, 0.01f, "%.2f");
            ch |= row_float("Up Offset", &rl->inst_up, 1.0f, -1e5f, 1e5f, "%.0f cm");
            ch |= row_float("Scale Min", &rl->inst_scale_min, 0.01f, 0.001f, 100.0f, "%.2f");
            ch |= row_float("Scale Max", &rl->inst_scale_max, 0.01f, 0.001f, 100.0f, "%.2f");
            static const char* const al[SPLINE_ALIGN_COUNT] = { "Upright", "Along the Spline", "As the Actor" };
            ch |= combo_cell("Align", &rl->inst_align, al, SPLINE_ALIGN_COUNT);
            uint32_t co = (rl->flags & SPLINE_F_MESH_COLLIDE) != 0;
            if (row_check("Instances Collide", &co)) rl->flags = co ? rl->flags | SPLINE_F_MESH_COLLIDE : rl->flags & ~SPLINE_F_MESH_COLLIDE, ch = true;
        }
        igEndTable();
    }
    return ch;
}

// --- The test scene (notas-dev/splines.md): a road bent along a hill (the example road, pieces by length), a pipe on
// custom tangents with roll, a fence of instances, three cables (between two poles, from a pole to a swinging arm, a
// ribbon in the wind) and a cable dragged over a box (its collision). --play: the arm turns (the cable follows it); the
// check: the pieces bent on the GPU, the cables hang (sag) without stretching, the end on the arm follows it, the cost ---
static Entity g_spl_arm, g_spl_cable_arm, g_spl_cable_poles, g_spl_cable_box;
static uint32_t g_spl_frames;
static double g_spl_ms_sum;
static float g_spl_follow_max;

static Entity spl_shape(World* w, const char* name, uint32_t mesh, const float at[3], const float sc[3], const float col[3], uint32_t mob) {
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
    if (e == ENTITY_NONE) return e;
    snprintf(((CName*)ecs_get(w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    memset(t, 0, sizeof *t);
    memcpy(t->location, at, 12), memcpy(t->scale, sc, 12), t->flags = mob;
    CStaticMesh* sm = ecs_get(w, e, COMP_STATIC_MESH);
    ecs_component_defaults(COMP_STATIC_MESH, sm);
    sm->mesh = mesh, sm->cast_shadow = 1, sm->roughness = 0.6f;
    memcpy(sm->base_color, col, 12);
    return e;
}

static void splines_scene(Editor* ed) {
    ecs_shutdown(&ed->world);
    ecs_init(&ed->world);
    ed->selected = ENTITY_NONE;
    uint32_t road = spline_road_mesh(ed), pipe = spline_pipe_mesh(ed); // (before the actors: the meshes reload)
    World* w = &ed->world;
    editor_sky_preset(ed, 0, 0.0);
    const float grey[3] = { 0.45f, 0.45f, 0.46f }, wood[3] = { 0.25f, 0.16f, 0.09f }, red[3] = { 0.55f, 0.08f, 0.06f };
    Entity fl = spl_shape(w, "Floor", SHAPE_PLANE, (float[3]){ 0, 0, 0 }, (float[3]){ 80, 80, 1 }, grey, MOBILITY_STATIC);
    ((CStaticMesh*)ecs_get(w, fl, COMP_STATIC_MESH))->material = content_material("M_Grid.rmat");
    // the road: over a little hill, the example road in pieces of 2 m
    Entity rd = spline_actor(w, "Road", SPLINE_KIND_SPLINE, (float[3]){ -1500, -600, 0 });
    CRail* r = ecs_get(w, rd, COMP_SPLINE);
    const float rp[5][3] = { { 0, 0, 2 }, { 800, 300, 120 }, { 1600, 0, 260 }, { 2400, -500, 120 }, { 3200, -300, 2 } };
    r->npoints = 5, memcpy(r->points, rp, sizeof rp);
    r->mesh_on = 1, r->mesh = road, r->mesh_axis = 0, r->mesh_length = 200.0f, r->mesh_material = content_material("M_Grid.rmat");
    r->roll[2] = 8.0f; // (banked at the top)
    // the pipe: custom tangents (a loop up), a scale growing to the end
    Entity pp = spline_actor(w, "Pipe", SPLINE_KIND_SPLINE, (float[3]){ -1200, 700, 60 });
    r = ecs_get(w, pp, COMP_SPLINE);
    const float pq[4][3] = { { 0, 0, 0 }, { 600, 0, 200 }, { 1200, 300, 80 }, { 1800, 300, 80 } };
    r->npoints = 4, memcpy(r->points, pq, sizeof pq);
    spline_set_point_type(r, 1, SPLINE_POINT_CUSTOM);
    r->leave[1][0] = r->arrive[1][0] = 900.0f, r->leave[1][2] = r->arrive[1][2] = 0.0f;
    r->ptype[2] = SPLINE_POINT_LINEAR;
    r->pscale[3][0] = r->pscale[3][1] = 1.8f;
    r->mesh_on = 1, r->mesh = pipe, r->mesh_axis = 0, r->mesh_color[0] = 0.75f, r->mesh_color[1] = 0.45f, r->mesh_color[2] = 0.15f, r->mesh_metallic = 0.8f, r->mesh_roughness = 0.35f;
    // the fence: posts every 1.5 m, a little random, along a closed loop
    Entity fe = spline_actor(w, "Fence", SPLINE_KIND_SPLINE, (float[3]){ 600, 900, 0 });
    r = ecs_get(w, fe, COMP_SPLINE);
    const float fq[4][3] = { { 0, 0, 0 }, { 900, -100, 0 }, { 1000, 700, 0 }, { 100, 800, 0 } };
    r->npoints = 4, r->closed = 1, memcpy(r->points, fq, sizeof fq);
    r->inst_on = 1, r->inst_mesh = SHAPE_CYLINDER, r->inst_spacing = 150.0f, r->inst_scale_min = 0.9f, r->inst_scale_max = 1.15f;
    r->inst_scale3[0] = r->inst_scale3[1] = 0.12f, r->inst_scale3[2] = 1.2f, r->inst_up = 60.0f;
    r->inst_jitter = 8.0f, r->inst_yaw_jitter = 20.0f, r->inst_seed = 7, memcpy(r->inst_color, wood, 12);
    // the poles and the cables between them
    Entity p1 = spl_shape(w, "PoleA", SHAPE_CYLINDER, (float[3]){ -600, -1500, 300 }, (float[3]){ 0.25f, 0.25f, 6.0f }, wood, MOBILITY_STATIC);
    Entity p2 = spl_shape(w, "PoleB", SHAPE_CYLINDER, (float[3]){ 700, -1500, 300 }, (float[3]){ 0.25f, 0.25f, 6.0f }, wood, MOBILITY_STATIC);
    (void)p1, (void)p2;
    Entity c1 = spline_actor(w, "CablePoles", SPLINE_KIND_CABLE, (float[3]){ 0, 0, 0 });
    r = ecs_get(w, c1, COMP_SPLINE);
    snprintf(r->cable_start_actor, 32, "PoleA"), snprintf(r->cable_end_actor, 32, "PoleB");
    r->cable_start_offset[2] = r->cable_end_offset[2] = 48.0f, r->cable_end_offset[0] = 0.0f; // (the poles' tops: 50 in their scale)
    r->cable_length = 1450.0f, r->cable_segments = 32, r->cable_width = 4.0f;
    g_spl_cable_poles = c1;
    // the arm (turned in Play) and a cable from the pole to it
    Entity arm = spl_shape(w, "Arm", SHAPE_CUBE, (float[3]){ 700, -900, 450 }, (float[3]){ 3.0f, 0.2f, 0.2f }, red, MOBILITY_MOVABLE);
    CRotating* rot = NULL;
    ecs_add(w, arm, COMP_ROTATING);
    rot = ecs_get(w, arm, COMP_ROTATING);
    if (rot) rot->rate[1] = 60.0f;
    g_spl_arm = arm;
    Entity c2 = spline_actor(w, "CableArm", SPLINE_KIND_CABLE, (float[3]){ 0, 0, 0 });
    r = ecs_get(w, c2, COMP_SPLINE);
    snprintf(r->cable_start_actor, 32, "PoleB"), snprintf(r->cable_end_actor, 32, "Arm");
    r->cable_start_offset[2] = 48.0f, r->cable_end_offset[0] = 50.0f, r->cable_length = 900.0f, r->cable_segments = 24;
    r->cable_color[0] = 0.6f, r->cable_color[1] = 0.5f, r->cable_color[2] = 0.1f;
    g_spl_cable_arm = c2;
    // a ribbon in the wind from a pole's top, its end free
    Entity c3 = spline_actor(w, "Ribbon", SPLINE_KIND_CABLE, (float[3]){ 0, 0, 0 });
    r = ecs_get(w, c3, COMP_SPLINE);
    snprintf(r->cable_start_actor, 32, "PoleA"), r->cable_start_offset[2] = 40.0f, r->cable_end_free = 1, r->cable_length = 400.0f;
    r->cable_render = CABLE_DRAW_RIBBON, r->cable_width = 12.0f, r->cable_wind = 6.0f, r->cable_wind_dir = 30.0f, r->cable_segments = 20;
    r->cable_color[0] = 0.7f, r->cable_color[1] = 0.1f, r->cable_color[2] = 0.1f;
    // a cable over a box (its collision): held at both sides of it, laid over its top
    spl_shape(w, "Crate", SHAPE_CUBE, (float[3]){ -200, 300, 60 }, (float[3]){ 1.2f, 1.2f, 1.2f }, wood, MOBILITY_STATIC);
    Entity c4 = spline_actor(w, "CableBox", SPLINE_KIND_CABLE, (float[3]){ -500, 300, 300 });
    r = ecs_get(w, c4, COMP_SPLINE);
    r->cable_end_offset[0] = 600.0f, r->cable_end_offset[2] = 0.0f, r->cable_length = 700.0f, r->cable_segments = 32;
    g_spl_cable_box = c4;
    g_spl_frames = 0, g_spl_ms_sum = 0.0, g_spl_follow_max = 0.0f;
    ed->cam.pos[0] = -2200.0f, ed->cam.pos[1] = -2600.0f, ed->cam.pos[2] = 900.0f, ed->cam.yaw = 45.0f, ed->cam.pitch = -15.0f;
    memcpy(ed->pie_cam.pos, ed->cam.pos, 12), ed->pie_cam.yaw = ed->cam.yaw, ed->pie_cam.pitch = ed->cam.pitch;
}

void editor_splines_tick(Editor* ed, double dt) {
    (void)dt;
    if (!ed->playing) return;
    g_spl_frames++;
    g_spl_ms_sum += spline_stats()->update_ms;
    float pts[CABLE_MAX_SEGMENTS + 1][3], rows[3][4];
    uint32_t n = cable_points(&ed->world, g_spl_cable_arm, pts, CABLE_MAX_SEGMENTS + 1);
    const CTransform* t = ecs_get(&ed->world, g_spl_arm, COMP_TRANSFORM);
    if (n >= 2 && t && g_spl_frames > 3) { // (the end on the arm's tip, where the arm is now)
        render_transform_rows(t, rows);
        float tip[3];
        for (int k = 0; k < 3; k++) tip[k] = rows[k][0] * 50.0f + rows[k][3];
        float d = sqrtf((pts[n - 1][0] - tip[0]) * (pts[n - 1][0] - tip[0]) + (pts[n - 1][1] - tip[1]) * (pts[n - 1][1] - tip[1]) + (pts[n - 1][2] - tip[2]) * (pts[n - 1][2] - tip[2]));
        if (d > g_spl_follow_max) g_spl_follow_max = d;
    }
}

bool editor_splines_check(Editor* ed) {
    World* w = &ed->world;
    bool ok = true;
    const SplineStats* st = spline_stats();
    uint32_t skins = 0, bent = 0, verts = 0;
    double skin_ms = 0.0;
    render_skin_stats(&skins, &bent, &verts, &skin_ms);
    // the cable between the poles: it sags, and no segment stretched past 3% (inextensible at Stiffness 1)
    float pts[CABLE_MAX_SEGMENTS + 1][3];
    uint32_t n = cable_points(w, g_spl_cable_poles, pts, CABLE_MAX_SEGMENTS + 1);
    float sag = 0.0f, stretch = 0.0f, len = 0.0f;
    bool finite = n >= 2;
    for (uint32_t i = 0; i < n; i++) {
        for (int k = 0; k < 3; k++) finite &= isfinite(pts[i][k]) != 0;
        float line = pts[0][2] + (pts[n - 1][2] - pts[0][2]) * (float)i / (float)(n - 1);
        sag = fmaxf(sag, line - pts[i][2]);
        if (i) len += sqrtf((pts[i][0] - pts[i - 1][0]) * (pts[i][0] - pts[i - 1][0]) + (pts[i][1] - pts[i - 1][1]) * (pts[i][1] - pts[i - 1][1]) + (pts[i][2] - pts[i - 1][2]) * (pts[i][2] - pts[i - 1][2]));
    }
    const CRail* cr = ecs_get(w, g_spl_cable_poles, COMP_SPLINE);
    if (cr && cr->cable_length > 0.0f) stretch = (len - cr->cable_length) / cr->cable_length * 100.0f;
    // the cable over the box: no particle inside it (the box: 120 cm at -200, 300, 60)
    uint32_t nb = cable_points(w, g_spl_cable_box, pts, CABLE_MAX_SEGMENTS + 1), inside = 0;
    for (uint32_t i = 1; i + 1 < nb; i++)
        inside += fabsf(pts[i][0] + 200.0f) < 58.0f && fabsf(pts[i][1] - 300.0f) < 58.0f && pts[i][2] > 2.0f && pts[i][2] < 118.0f;
    double cpu = g_spl_frames ? g_spl_ms_sum / g_spl_frames : st->update_ms;
    printf("[RealEngine] splines: %u frames, %u splines, %u Spline Mesh pieces (%u skins bent, %u vertices, CPU %.3f ms), %u instances, %u cables (%u particles, "
           "%u contacts), update %.3f ms (cable peak %.3f ms), rebuilt %u (%.2f ms), sag %.0f cm, stretch %.2f%%, arm end %.2f cm off, %u in the box\n",
           g_spl_frames, st->splines, st->meshes, bent, verts, skin_ms, st->instances, st->cables, st->particles, st->contacts, cpu, st->cable_ms_peak,
           st->rebuilt, st->rebuild_ms, sag, stretch, g_spl_follow_max, inside);
    if (st->meshes < 10 || st->instances < 10 || st->cables < 4) printf("[RealEngine] FAIL: splines: the pieces were not made\n"), ok = false;
    if (!finite || sag < 40.0f) printf("[RealEngine] FAIL: splines: the cable between the poles does not hang (sag %.1f cm)\n", sag), ok = false;
    if (fabsf(stretch) > 3.0f) printf("[RealEngine] FAIL: splines: the cable stretched %.2f%%\n", stretch), ok = false;
    if (ed->playing || g_spl_frames) {
        if (g_spl_follow_max > 1.0f) printf("[RealEngine] FAIL: splines: the cable's end left the arm (%.2f cm)\n", g_spl_follow_max), ok = false;
    }
    if (inside) printf("[RealEngine] FAIL: splines: %u particles of the cable inside the box\n", inside), ok = false;
    if (cpu > 0.5) printf("[RealEngine] FAIL: splines: the update costs %.3f ms (> 0.5)\n", cpu), ok = false;
    return ok;
}
