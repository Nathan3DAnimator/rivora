// The haunted house (--scene haunted; notas-dev/demos.md "Casa assombrada"): an original survival-horror scene to measure
// the engine on a realistic interior: the GI test's house (10 x 8 m, two rooms and a door) as a Victorian house left to
// rot, at night. CC0 assets only (Poly Haven models at 2K, ambientCG materials at 2K: their list in Sources/LICENSES.txt),
// read from the folder next to the project (REALENGINE_HAUNTED_SOURCES, else <projects>/CasaAssombrada_fontes, made by
// a download script); two characters of the engine's own (the female Mannequin hiding, a stretched figure at the door).
// The level is saved into the project and made its startup map (editor and game).
// Included by editor.c after demo_games.inc (the kit, the house helpers).

static uint32_t g_hh_floor, g_hh_paper, g_hh_panel, g_hh_ceiling, g_hh_bricks, g_hh_rug, g_hh_ground;

// A static box from (x0, y0, z0) to (x1, y1, z1) (cm), of a material (World Aligned: any size tiles alike)
static Entity hh_box(Editor* ed, float x0, float y0, float z0, float x1, float y1, float z1, uint32_t mat, const float col[3]) {
    const float grey[3] = { 0.5f, 0.5f, 0.5f };
    Entity e = mesh_actor(ed, SHAPE_CUBE, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f, col ? col : grey, 0, 0.8f);
    CTransform* t = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
    if (t) t->scale[0] = (x1 - x0) / 100.0f, t->scale[1] = (y1 - y0) / 100.0f, t->scale[2] = (z1 - z0) / 100.0f, t->flags = MOBILITY_STATIC;
    if (e && mat) ((CStaticMesh*)ecs_get(&ed->world, e, COMP_STATIC_MESH))->material = mat;
    return e;
}

// A wall along x (at y, from x0 to x1, 20 cm thick, 300 high) with windows (centers along x, 110 wide, sill 90, top 240):
// the wallpaper over dark wood paneling up to 95 cm and a rail on the room's side (side: +1 the room at +y, -1 at -y)
static void hh_wall_x(Editor* ed, float y, float x0, float x1, const float* win, int nwin, int side) {
    float a = x0;
    for (int i = 0; i <= nwin; i++) {
        float b = i < nwin ? win[i] - 55.0f : x1;
        if (b > a) hh_box(ed, a, y - 10, 0, b, y + 10, 300, g_hh_paper, NULL);
        if (i < nwin) {
            hh_box(ed, win[i] - 55, y - 10, 0, win[i] + 55, y + 10, 90, g_hh_paper, NULL);   // under the window
            hh_box(ed, win[i] - 55, y - 10, 240, win[i] + 55, y + 10, 300, g_hh_paper, NULL); // over it
            // its frame and cross (dark wood), the sill
            const float wood[3] = { 0.12f, 0.08f, 0.05f };
            hh_box(ed, win[i] - 55, y - 4, 88, win[i] + 55, y + 4 + 8.0f * (float)side, 92, g_hh_panel, wood);
            hh_box(ed, win[i] - 3, y - 3, 90, win[i] + 3, y + 3, 240, g_hh_panel, wood);
            hh_box(ed, win[i] - 55, y - 3, 168, win[i] + 55, y + 3, 174, g_hh_panel, wood);
            a = win[i] + 55;
        }
    }
    float ry = y + 10.0f * (float)side; // the room's face
    hh_box(ed, x0, side > 0 ? ry : ry - 2.5f, 0, x1, side > 0 ? ry + 2.5f : ry, 95, g_hh_panel, NULL);
    hh_box(ed, x0, side > 0 ? ry : ry - 4.5f, 93, x1, side > 0 ? ry + 4.5f : ry, 100, g_hh_panel, NULL);
}
// The same along y (at x, from y0 to y1); a door (its center along y, 110 wide, 220 high) or none (NAN)
static void hh_wall_y(Editor* ed, float x, float y0, float y1, float door, int side) {
    if (isnan(door)) hh_box(ed, x - 10, y0, 0, x + 10, y1, 300, g_hh_paper, NULL);
    else {
        hh_box(ed, x - 10, y0, 0, x + 10, door - 55, 300, g_hh_paper, NULL);
        hh_box(ed, x - 10, door + 55, 0, x + 10, y1, 300, g_hh_paper, NULL);
        hh_box(ed, x - 10, door - 55, 220, x + 10, door + 55, 300, g_hh_paper, NULL);
        const float wood[3] = { 0.12f, 0.08f, 0.05f }; // the door's frame
        hh_box(ed, x - 14, door - 61, 0, x + 14, door - 55, 226, g_hh_panel, wood);
        hh_box(ed, x - 14, door + 55, 0, x + 14, door + 61, 226, g_hh_panel, wood);
        hh_box(ed, x - 14, door - 61, 220, x + 14, door + 61, 226, g_hh_panel, wood);
    }
    for (int s = -1; s <= 1; s += 2) { // the paneling on both faces (the inner wall) or the room's (side)
        if (side != 0 && s != side) continue;
        float rx = x + 10.0f * (float)s;
        float pa = s > 0 ? rx : rx - 2.5f, pb = s > 0 ? rx + 2.5f : rx, ra = s > 0 ? rx : rx - 4.5f, rb = s > 0 ? rx + 4.5f : rx;
        if (isnan(door)) {
            hh_box(ed, pa, y0, 0, pb, y1, 95, g_hh_panel, NULL), hh_box(ed, ra, y0, 93, rb, y1, 100, g_hh_panel, NULL);
        } else {
            hh_box(ed, pa, y0, 0, pb, door - 61, 95, g_hh_panel, NULL), hh_box(ed, ra, y0, 93, rb, door - 61, 100, g_hh_panel, NULL);
            hh_box(ed, pa, door + 61, 0, pb, y1, 95, g_hh_panel, NULL), hh_box(ed, ra, door + 61, 93, rb, y1, 100, g_hh_panel, NULL);
        }
    }
}

// A prop's size (its parts' bounds at scale 1, cm): printed once (placing by sight)
static void hh_size(Editor* ed, const char* id, const KitProp* p) {
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < p->n; i++) {
        const CTransform* t = ecs_get(&ed->world, p->parts[i], COMP_TRANSFORM);
        const CStaticMesh* sm = ecs_get(&ed->world, p->parts[i], COMP_STATIC_MESH);
        if (!t || !sm) continue;
        float a[3], b[3];
        render_mesh_bounds(render_mesh_slot(sm->mesh), a, b);
        for (int k = 0; k < 3; k++) {
            float lo = t->location[k] + a[k] * t->scale[k], hi = t->location[k] + b[k] * t->scale[k];
            mn[k] = fminf(mn[k], fminf(lo, hi)), mx[k] = fmaxf(mx[k], fmaxf(lo, hi));
        }
    }
    printf("[RealEngine] Haunted: %s %u parts, %.0f x %.0f x %.0f cm (x %.0f..%.0f, y %.0f..%.0f)\n", id, p->n, mx[0] - mn[0], mx[1] - mn[1],
           mx[2] - mn[2], mn[0], mx[0], mn[1], mx[1]);
}

// One prop placed n times (x, y, z, yaw, scale each); a light's own fixture casts no shadow (the chandelier's cups right
// over its flames drew a hard black disk on the ceiling)
static void hh_props_ex(Editor* ed, const char* id, int n, const float (*at)[5], bool shadow) {
    KitProp p = kit_prop(ed, "Casa", id);
    if (!p.n) {
        printf("[RealEngine] FAIL: haunted house: %s missing\n", id);
        return;
    }
    hh_size(ed, id, &p);
    for (uint32_t i = 0; i < p.n && !shadow; i++) {
        CStaticMesh* sm = ecs_get(&ed->world, p.parts[i], COMP_STATIC_MESH);
        if (sm) sm->cast_shadow = 0;
    }
    for (int i = 0; i < n; i++) kit_place(ed, &p, at[i][0], at[i][1], at[i][2], at[i][3], at[i][4], false);
    kit_done(ed, &p);
}
static void hh_props(Editor* ed, const char* id, int n, const float (*at)[5]) { hh_props_ex(ed, id, n, at, true); }

static Entity hh_light(Editor* ed, const char* name, float x, float y, float z, const float c[3], float cd, float radius, float src, bool shadows) {
    Entity e = hz_light(ed, name, x, y, z, c[0], c[1], c[2], cd, radius, shadows);
    CLight* l = e ? ecs_get(&ed->world, e, COMP_LIGHT) : NULL;
    if (l) l->source_radius = src;
    return e;
}

static Entity hh_character(Editor* ed, const char* name, const char* mesh, const char* clip, float x, float y, float yaw,
                           const float scale[3], float rate) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_SKELETAL_MESH));
    if (e == ENTITY_NONE) return e;
    set_name(w, e, name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = 0, t->rotation[1] = yaw;
    for (int k = 0; k < 3; k++) t->scale[k] = scale[k];
    t->flags = MOBILITY_MOVABLE;
    CSkeletalMesh* sk = ecs_get(w, e, COMP_SKELETAL_MESH);
    const SkelMesh* m = anim_mesh_name(mesh);
    const AnimSeq* a = anim_seq_name(clip);
    sk->mesh = m ? m->hash : 0, sk->anim = a ? a->hash : 0, sk->play_rate = rate, sk->loop = 1;
    if (!m || !a) printf("[RealEngine] FAIL: haunted house: %s / %s missing\n", mesh, clip);
    return e;
}

static void haunted_scene(Editor* ed) {
    World* w = &ed->world;
    if (!project_file()[0]) {
        printf("[RealEngine] FAIL: haunted house: needs a project (--new-project folder/Name)\n");
        return;
    }
    char src[1100];
    const char* env = getenv("REALENGINE_HAUNTED_SOURCES");
    if (env && env[0]) snprintf(src, sizeof src, "%s", env);
    else snprintf(src, sizeof src, "%s/../../CasaAssombrada_fontes", content_root());
    if (!re_file_stat(src, NULL, NULL, NULL)) {
        printf("[RealEngine] FAIL: haunted house: its sources not found (%s)\n", src);
        return;
    }
    for (int p = 0; p < ANIM_PACK_COUNT; p++) anim_make_pack(p); // (the Mannequins and their clips)
    template_ground(ed, 60.0f);
    editor_sky_preset(ed, 6, 0.0); // night: the moon
    { // (no clouds: a closed house at night never shows them, and their march cost 1.6 ms)
        Entity cl = find_actor(w, COMP_CLOUD, 0);
        if (cl) ecs_destroy(w, cl);
    }
    kit_source(src, true);
    // the materials (ambientCG 2K, World Aligned): faded olive wallpaper, dark wood paneling, worn dark floorboards,
    // old plaster, a wine-red rug, old bricks, the earth outside
    g_hh_paper = kit_material("Casa", "Wallpaper002B", 180.0f, (float[3]){ 0.42f, 0.45f, 0.34f }, 1.0f);
    g_hh_panel = kit_material("Casa", "Wood049", 120.0f, (float[3]){ 0.45f, 0.32f, 0.24f }, 0.9f);
    g_hh_floor = kit_material("Casa", "WoodFloor041", 220.0f, (float[3]){ 0.8f, 0.72f, 0.65f }, 1.0f);
    g_hh_ceiling = kit_material("Casa", "Plaster001", 300.0f, (float[3]){ 0.5f, 0.48f, 0.44f }, 1.0f);
    g_hh_bricks = kit_material("Casa", "Bricks094", 110.0f, (float[3]){ 0.75f, 0.6f, 0.55f }, 1.0f);
    g_hh_rug = kit_material("Casa", "Carpet012", 90.0f, (float[3]){ 0.55f, 0.1f, 0.1f }, 1.0f);
    g_hh_ground = kit_material("Casa", "Ground037", 300.0f, (float[3]){ 0.6f, 0.62f, 0.58f }, 1.0f);
    Entity floor = find_actor(w, COMP_STATIC_MESH, 0);
    for (uint32_t ai = 0; ai < w->archetype_count && g_hh_ground; ai++) { // the template's ground: the earth outside
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_STATIC_MESH)) || !(a->mask & COMP_BIT(COMP_NAME))) continue;
        CStaticMesh* m = ecs_arch_column(a, COMP_STATIC_MESH);
        const CName* nm = ecs_arch_column(a, COMP_NAME);
        for (uint32_t i = 0; i < a->count; i++)
            if (!strcmp(nm[i].text, "Floor")) m[i].material = g_hh_ground;
    }
    (void)floor;
    // the house: the floor, the ceiling, the outer walls (windows south and north), the inner wall and its door
    hh_box(ed, -520, -420, -12, 520, 420, 0.5f, g_hh_floor, NULL);
    hh_box(ed, -520, -420, 300, 520, 420, 320, g_hh_ceiling, NULL);
    hh_box(ed, -540, -440, 320, 540, 440, 340, g_hh_bricks, NULL); // (the roof slab over it all: no light from above)
    const float south[3] = { -340, -150, 300 }, north[1] = { 300 };
    hh_wall_x(ed, -410, -520, 520, south, 3, 1);
    hh_wall_x(ed, 410, -520, 520, north, 1, -1);
    hh_wall_y(ed, -510, -400, 400, NAN, 1);
    hh_wall_y(ed, 510, -400, 400, NAN, -1);
    hh_wall_y(ed, 100, -400, 400, 0.0f, 0);
    // the fireplace (west wall): the chimney breast of bricks, the firebox's opening, the hearth, the mantel
    const float soot[3] = { 0.02f, 0.018f, 0.015f }, wood[3] = { 0.14f, 0.09f, 0.06f };
    hh_box(ed, -500, -115, 0, -455, -60, 300, g_hh_bricks, NULL);
    hh_box(ed, -500, 60, 0, -455, 115, 300, g_hh_bricks, NULL);
    hh_box(ed, -500, -60, 105, -455, 60, 300, g_hh_bricks, NULL);
    hh_box(ed, -500, -60, 0, -485, 60, 105, 0, soot); // (its back, black)
    hh_box(ed, -455, -130, 0, -415, 130, 5, g_hh_bricks, NULL);
    hh_box(ed, -462, -130, 105, -440, 130, 113, g_hh_panel, wood);
    Entity embers = hh_box(ed, -480, -35, 5, -462, 35, 13, 0, (float[3]){ 0.1f, 0.03f, 0.01f });
    if (embers) {
        CStaticMesh* m = ecs_get(w, embers, COMP_STATIC_MESH);
        m->emissive[0] = 6.0f, m->emissive[1] = 1.4f, m->emissive[2] = 0.2f;
        set_name(w, embers, "Embers");
    }
    // the rug under the dining table
    hh_box(ed, -360, -120, 0.5f, -40, 120, 2.0f, g_hh_rug, NULL);
    // the parlor (west room): the dining table and its chairs under the chandelier, the armchair and the rocking chair
    // by the fire, the bookshelves, the grandfather clock, the bust on its stand, the mirror over the mantel, portraits
    hh_props(ed, "dining_table", 1, (const float[][5]){ { -200, 0, 2, 90, 1 } });
    hh_props(ed, "dining_chair_02", 4, (const float[][5]){ { -270, -75, 2, 90, 1 }, { -130, -75, 2, 70, 1 }, { -270, 75, 2, 270, 1 }, { -110, 95, 2, 250, 1 } });
    hh_props_ex(ed, "Chandelier_01", 1, (const float[][5]){ { -200, 0, 205, 0, 1 } }, false);
    hh_props(ed, "ArmChair_01", 1, (const float[][5]){ { -360, -230, 0, 120, 1 } });
    hh_props(ed, "Rockingchair_01", 1, (const float[][5]){ { -350, 220, 0, 230, 1 } });
    hh_props(ed, "wooden_bookshelf_worn", 2, (const float[][5]){ { -330, 380, 0, 180, 1 }, { -120, 380, 0, 180, 1 } });
    hh_props(ed, "vintage_grandfather_clock_01", 1, (const float[][5]){ { 55, 360, 0, 180, 1 } });
    hh_props(ed, "side_table_tall_01", 1, (const float[][5]){ { 45, -350, 0, 0, 1 } });
    hh_props(ed, "marble_bust_01", 1, (const float[][5]){ { 45, -350, 80, 160, 1 } });
    hh_props(ed, "ornate_mirror_01", 1, (const float[][5]){ { -458, 0, 150, 90, 1 } });
    hh_props(ed, "mantel_clock_01", 1, (const float[][5]){ { -452, 0, 113, 90, 1 } });
    hh_props(ed, "brass_candleholders", 2, (const float[][5]){ { -452, -95, 113, 90, 1 }, { -452, 95, 113, 90, 1 } });
    hh_props(ed, "fancy_picture_frame_01", 1, (const float[][5]){ { -245, -398, 150, 0, 1 } });
    hh_props(ed, "fancy_picture_frame_02", 1, (const float[][5]){ { 88, -230, 140, 270, 1 } });
    // on the table: books, bottles, a candlestick
    hh_props(ed, "book_encyclopedia_set_01", 1, (const float[][5]){ { -240, 20, 78, 30, 1 } });
    hh_props(ed, "wine_bottles_01", 1, (const float[][5]){ { -160, -25, 78, 0, 1 } });
    hh_props(ed, "wooden_candlestick", 1, (const float[][5]){ { -200, 10, 78, 0, 1 } });
    // the bedroom (east room): the gothic bed, the nightstand and its lamp, the cabinet, the commode, a suitcase, the
    // wheelchair facing the wall, a portrait over the bed
    hh_props(ed, "GothicBed_01", 1, (const float[][5]){ { 400, 120, 0, 270, 1 } });
    hh_props(ed, "ClassicNightstand_01", 1, (const float[][5]){ { 460, -60, 0, 270, 1 } });
    hh_props(ed, "vintage_oil_lamp", 2, (const float[][5]){ { 460, -60, 60, 0, 1 }, { -150, -385, 92, 0, 1 } });
    hh_props(ed, "GothicCabinet_01", 1, (const float[][5]){ { 230, 375, 0, 180, 1 } });
    hh_props(ed, "GothicCommode_01", 1, (const float[][5]){ { 330, -380, 0, 0, 1 } });
    hh_props(ed, "vintage_suitcase", 1, (const float[][5]){ { 170, -320, 0, 20, 1 } });
    hh_props(ed, "wheelchair_01", 1, (const float[][5]){ { 180, 260, 0, 140, 1 } });
    hh_props(ed, "hanging_picture_frame_03", 1, (const float[][5]){ { 497, 120, 170, 270, 1 } });
    // the rats
    hh_props(ed, "street_rat", 3, (const float[][5]){ { -400, 60, 5, 200, 1 }, { -150, 160, 0, 40, 1 }, { 300, -100, 0, 300, 1 } });
    // outside: dead trunks in the moonlight past the windows
    hh_props(ed, "dead_tree_trunk", 3, (const float[][5]){ { -600, -900, 0, 30, 1.4f }, { 200, -1100, 0, 200, 1.2f }, { 700, 900, 0, 120, 1.3f } });
    // the light: the moon through the south windows (cold, its shafts in the dust), the chandelier's candles (warm, soft
    // shadows), the fire, the candles on the mantel and the table, the oil lamp by the bed
    Entity moon = find_actor(w, COMP_LIGHT, LIGHT_DIRECTIONAL);
    CTransform* mt = moon ? ecs_get(w, moon, COMP_TRANSFORM) : NULL;
    if (mt) mt->rotation[0] = -28.0f, mt->rotation[1] = 75.0f;
    Entity fog = find_actor(w, COMP_FOG, 0);
    CFog* fg = fog ? ecs_get(w, fog, COMP_FOG) : NULL;
    if (fg) fg->density = 0.02f, fg->volumetric = 1, fg->volumetric_distance = 2000.0f;
    const float candle[3] = { 1.0f, 0.62f, 0.3f }, fire[3] = { 1.0f, 0.38f, 0.1f }, lamp[3] = { 1.0f, 0.55f, 0.24f };
    hh_light(ed, "Chandelier", -200, 0, 222, candle, 70.0f, 900.0f, 0.0f, true); // (candle flames: a cm wide, sharp shadows; one exact ray. Soft: +1.7 ms)
    hh_light(ed, "Fire", -470, 0, 35, fire, 90.0f, 560.0f, 0.0f, false); // (low in its opening: the chandelier gives the room its shadows) // (a point: one exact ray; the chandelier is the one area light, its shadows filtered)
    hh_light(ed, "MantelCandleL", -452, -95, 140, candle, 4.0f, 180.0f, 0.0f, false);
    hh_light(ed, "MantelCandleR", -452, 95, 140, candle, 4.0f, 180.0f, 0.0f, false);
    hh_light(ed, "TableCandle", -200, 10, 112, candle, 6.0f, 220.0f, 0.0f, false);
    hh_light(ed, "BedLamp", 460, -60, 95, lamp, 25.0f, 420.0f, 0.0f, true); // (its radius: the bedroom, not the parlor through the door)
    hh_light(ed, "WindowLamp", -150, -385, 120, lamp, 8.0f, 260.0f, 0.0f, false);
    // the post process: a fixed exposure for the night, cold, desaturated, a vignette and film grain
    Entity pp = spawn_actor(ed, SPAWN_POST, 0, (float[3]){ 0, 0, 0 }, 0.0);
    CPostProcess* pv = pp ? ecs_get(w, pp, COMP_POST_PROCESS) : NULL;
    if (pv) {
        pv->flags = PPV_ENABLED | PPV_UNBOUND;
        const int fs[] = { PP_EXPOSURE_MIN_EV, PP_EXPOSURE_MAX_EV, PP_VIGNETTE, PP_GRAIN, PP_SATURATION, PP_TEMPERATURE, PP_CONTRAST };
        const float vs[] = { 1.5f, 1.5f, 0.55f, 0.1f, 0.72f, 7400.0f, 1.08f };
        for (int k = 0; k < 7; k++) pv->overrides |= 1u << fs[k], pv->s.v[fs[k]] = vs[k];
    }
    // the characters: she hides behind the table; it stands in the doorway, too tall, too thin, slow
    hh_character(ed, "Protagonista", "SK_Mannequin_Female", "MM_CrouchIdle", -70, -150, 150.0f, (float[3]){ 1, 1, 1 }, 1.0f);
    hh_character(ed, "Criatura", "SK_Mannequin_Male", "MM_Walk", 135, 10, 180.0f, (float[3]){ 0.82f, 0.82f, 1.3f }, 0.45f);
    // the player's start (Play: the First Person character) and the editor's view
    Entity ps = find_actor(w, COMP_PLAYER_START, 0);
    CTransform* pst = ps ? ecs_get(w, ps, COMP_TRANSFORM) : NULL;
    if (pst) pst->location[0] = -400, pst->location[1] = -300, pst->location[2] = 100, pst->rotation[1] = 30;
    ed->selected = ENTITY_NONE;
    ed->cam.pos[0] = -440.0f; ed->cam.pos[1] = -330.0f; ed->cam.pos[2] = 160.0f;
    ed->cam.yaw = 32.0f; ed->cam.pitch = -6.0f;
    kit_source(NULL, false);
    // the level into the project, its startup map
    ProjectSettings* prj = project();
    char path[1200];
    snprintf(path, sizeof path, "%s/Maps", content_root());
    re_make_dirs(path);
    snprintf(path, sizeof path, "%s/Maps/CasaAssombrada.rlevel", content_root());
    if (editor_save_level(ed, path, re_time_seconds())) {
        snprintf(prj->editor_startup_map, sizeof prj->editor_startup_map, "/Content/Maps/CasaAssombrada.rlevel");
        snprintf(prj->game_default_map, sizeof prj->game_default_map, "/Content/Maps/CasaAssombrada.rlevel");
        prj->game_scalability = 2; // High: the ray traced GI, shadows and reflections at > 100 fps here (Epic ~85 at 1440p)
        project_save(NULL);
        printf("[RealEngine] Haunted house saved: %s\n", path);
    } else printf("[RealEngine] FAIL: haunted house: the level not saved\n");
    ed->dirty = false;
}
