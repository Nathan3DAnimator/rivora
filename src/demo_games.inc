// --- The demo games (included by editor.c): three templates that are small complete games made of the engine's own
// pieces, their logic in Blueprints (the Event Graph of the player's Character), so they can be opened and changed.
// RPG: a village, coins, a merchant who trades potions for gold (E), bandits with AI that attack, sword (LMB), potions (Q).
// Horror: a dark house at night, fog, a flashlight (F), keys that open locked doors, a monster with AI that hunts by
// sight and sound, the exit. Fighting: an arena, a rival with AI, punch (LMB), kick (RMB), block (Left Shift), K.O.
// The player's overlap shape (a capsule a little bigger than the body) tells what it touches: by the other actor's
// name (Key, Door, Exit, Monster, Coin, Merchant, Bandit, Rival) ---

// The graph helpers: nodes by id (the nodes array may move while it grows), wires and literals by pin name
static uint32_t dg_node(BpAsset* a, uint32_t kind, float x, float y, const char* text) {
    BpNode* n = bp_add_node(a, &a->graphs[0], kind, x, y, text, NULL);
    return n ? n->id : 0;
}
static int dg_pin(BpAsset* a, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(&a->graphs[0], id);
    for (int p = 0; n && p < n->npins; p++)
        if (n->pins[p].out == out && !strcmp(n->pins[p].name, name)) return p;
    return -1;
}
static void dg_wire(BpAsset* a, uint32_t from, const char* fp, uint32_t to, const char* tp) {
    int x = dg_pin(a, from, 1, fp), y = dg_pin(a, to, 0, tp);
    if (x >= 0 && y >= 0) bp_link(&a->graphs[0], from, (uint8_t)x, to, (uint8_t)y);
}
static void dg_num(BpAsset* a, uint32_t id, const char* pin, float v) {
    int p = dg_pin(a, id, 0, pin);
    BpNode* n = bp_node(&a->graphs[0], id);
    if (n && p >= 0) n->pins[p].lit.v[0] = v;
}
static void dg_text(BpAsset* a, uint32_t id, const char* pin, const char* s) {
    int p = dg_pin(a, id, 0, pin);
    BpNode* n = bp_node(&a->graphs[0], id);
    if (n && p >= 0) snprintf(n->pins[p].slit, sizeof n->pins[p].slit, "%s", s);
}
static void dg_var(BpAsset* a, const char* name, uint8_t type, float def) {
    if (a->nvars >= 64 || bp_var_index(a, name) >= 0) return;
    BpVar* v = &a->vars[a->nvars++];
    memset(v, 0, sizeof *v);
    snprintf(v->name, sizeof v->name, "%s", name);
    v->type = type, v->def.v[0] = def;
}
// exec -> Print String text -> returns the last print (to go on from it). A pin's text holds 63 bytes: a longer one
// becomes several prints, cut between words
static uint32_t dg_print(BpAsset* a, uint32_t after, const char* after_pin, float x, float y, const char* text) {
    uint32_t p = 0;
    while (*text) {
        size_t n = strlen(text);
        if (n > 60) {
            n = 60;
            while (n > 20 && text[n] != ' ') n--;
        }
        char part[64];
        memcpy(part, text, n);
        part[n] = 0;
        p = dg_node(a, BPK_PRINT, x, y, NULL);
        dg_text(a, p, "In String", part);
        dg_wire(a, after, after_pin, p, "");
        after = p, after_pin = "", x += 220.0f;
        text += n;
        while (*text == ' ') text++;
    }
    return p;
}
// Set var = var + delta (an Integer), after the exec; returns the Set
static uint32_t dg_add_int(BpAsset* a, uint32_t after, const char* after_pin, float x, float y, const char* var, float delta) {
    uint32_t g = dg_node(a, BPK_VAR_GET, x, y + 90, var), add = dg_node(a, BPK_IADD, x + 140, y + 90, NULL), s = dg_node(a, BPK_VAR_SET, x + 300, y, var);
    dg_wire(a, g, var, add, "A");
    dg_num(a, add, "B", delta);
    dg_wire(a, add, "", s, var);
    dg_wire(a, after, after_pin, s, "");
    return s;
}
// Set var = var + delta (a Float)
static uint32_t dg_add_float(BpAsset* a, uint32_t after, const char* after_pin, float x, float y, const char* var, uint32_t delta_node, float delta) {
    uint32_t g = dg_node(a, BPK_VAR_GET, x, y + 90, var), add = dg_node(a, BPK_ADD, x + 140, y + 90, NULL), s = dg_node(a, BPK_VAR_SET, x + 300, y, var);
    dg_wire(a, g, var, add, "A");
    if (delta_node) dg_wire(a, delta_node, "", add, "B");
    else dg_num(a, add, "B", delta);
    dg_wire(a, add, "", s, var);
    dg_wire(a, after, after_pin, s, "");
    return s;
}
// Branch on "Other Actor's name contains word"; the event's Other Actor pin feeds it
static uint32_t dg_name_is(BpAsset* a, uint32_t ev, uint32_t after, const char* after_pin, float x, float y, const char* word) {
    uint32_t dn = dg_node(a, BPK_GET_DISPLAY_NAME, x, y + 110, NULL), c = dg_node(a, BPK_STR_CONTAINS, x + 160, y + 110, NULL),
             br = dg_node(a, BPK_BRANCH, x + 340, y, NULL);
    dg_wire(a, ev, "Other Actor", dn, "Object");
    dg_wire(a, dn, "", c, "Search In");
    dg_text(a, c, "Substring", word);
    dg_wire(a, c, "", br, "Condition");
    dg_wire(a, after, after_pin, br, "");
    return br;
}
// Print, wait, play the map again (Open Level by Name)
static void dg_restart(BpAsset* a, uint32_t after, const char* after_pin, float x, float y, const char* text, const char* map) {
    uint32_t once = dg_node(a, BPK_DO_ONCE, x, y, NULL);
    dg_wire(a, after, after_pin, once, "");
    uint32_t p = dg_print(a, once, "Completed", x + 180, y, text);
    uint32_t d = dg_node(a, BPK_DELAY, x + 400, y, NULL);
    dg_num(a, d, "Duration", 4.0f);
    dg_wire(a, p, "", d, "");
    uint32_t o = dg_node(a, BPK_OPEN_LEVEL, x + 560, y, NULL);
    dg_text(a, o, "Level Name", map);
    dg_wire(a, d, "Completed", o, "");
}

// The player's overlap capsule (a sensor around the body: what it touches fires its overlap events)
static void dg_sensor(BpAsset* c, float radius, float half_height) {
    int boom = scs_index(c, "CameraBoom");
    int parent = boom >= 0 ? c->scs[boom].parent : BP_SCS_ROOT;
    if (scs_index(c, "OverlapSensor") >= 0) return;
    int s = bp_scs_add(c, SCS_CAPSULE, parent, "OverlapSensor");
    if (s < 0) return;
    CCollision* co = (CCollision*)c->scs[s].data;
    co->shape = COLLISION_CAPSULE, co->extent[0] = radius, co->extent[1] = half_height, co->generate_overlap_events = 1, co->block = 0;
}

// A level actor that is both seen and touched: a mesh with an overlap shape, named for the player's graph
static Entity dg_pickup(Editor* ed, const char* name, int shape, float x, float y, float z, const float color[3], float glow, float radius) {
    Entity e = mesh_actor(ed, shape, x, y, z, color, 0.6f, 0.35f);
    World* w = &ed->world;
    if (e == ENTITY_NONE) return e;
    set_name(w, e, name);
    CStaticMesh* m = ecs_get(w, e, COMP_STATIC_MESH);
    if (m) m->emissive[0] = color[0] * glow, m->emissive[1] = color[1] * glow, m->emissive[2] = color[2] * glow;
    ecs_add(w, e, COMP_COLLISION);
    CCollision* co = ecs_get(w, e, COMP_COLLISION);
    if (co) co->shape = COLLISION_SPHERE, co->extent[0] = radius, co->generate_overlap_events = 1, co->block = 0;
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    if (t) t->flags = MOBILITY_MOVABLE;
    return e;
}

static Entity dg_proto(Editor* ed, int kind, float x, float y, float sx, float sy, float sz, float yaw, bool ceiling, float door, const float color[3]) {
    Entity e = spawn_actor(ed, SPAWN_PROTO, kind, (float[3]){ x, y, 0 }, 0.0);
    CProto* p = e ? ecs_get(&ed->world, e, COMP_PROTO) : NULL;
    CTransform* t = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
    if (!p || !t) return e;
    p->size[0] = sx, p->size[1] = sy, p->size[2] = sz;
    p->flags = ceiling ? p->flags | PROTO_CEILING : p->flags & ~PROTO_CEILING;
    if (door > 0.0f) p->opening[0] = door, p->opening[1] = 230.0f, p->opening[2] = 0.0f;
    if (color) memcpy(p->color, color, 12);
    t->rotation[1] = yaw;
    return e;
}

static Entity dg_light(Editor* ed, float x, float y, float z, float r, float g, float b, float intensity, float radius) {
    Entity e = spawn_actor(ed, SPAWN_LIGHT, 1, (float[3]){ x, y, z }, 0.0);
    CLight* l = e ? ecs_get(&ed->world, e, COMP_LIGHT) : NULL;
    CTransform* t = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
    if (!l || !t) return e;
    t->location[2] = z;
    l->color[0] = r, l->color[1] = g, l->color[2] = b, l->intensity = intensity, l->attenuation_radius = radius, l->cast_shadows = 1;
    return e;
}

static Entity dg_ai(Editor* ed, const char* name, float x, float y, float yaw, const char* brain, const char* tree, float speed) {
    uint32_t mode = AI_MODE_NONE, asset = 0;
    for (uint32_t i = 0; brain && i < ai_brain_count(); i++)
        if (!strcmp(ai_brain_at(i)->name, brain)) mode = AI_MODE_BRAIN, asset = ai_brain_at(i)->hash;
    for (uint32_t i = 0; tree && !asset && i < ai_tree_count(); i++)
        if (!strcmp(ai_tree_at(i)->name, tree)) mode = AI_MODE_TREE, asset = ai_tree_at(i)->hash;
    Entity e = ai_character(ed, name, x, y, yaw, mode, asset, 2);
    CCharacter* c = e ? ecs_get(&ed->world, e, COMP_CHARACTER) : NULL;
    if (c && speed > 0.0f) c->max_walk_speed = speed;
    return e;
}

// The player: the Third Person Character (or the First Person one: the camera at the eyes), a sensor, its variables
static BpAsset* dg_player(Editor* ed, const char* folder, const char* name, bool first_person) {
    (void)ed;
    BpAsset* c = template_class(folder, name, "Character");
    if (!c) return NULL;
    if (first_person) {
        int mesh = scs_index(c, "Mesh");
        if (mesh >= 0) bp_scs_remove(c, mesh);
        int boom = scs_index(c, "CameraBoom");
        if (boom >= 0) {
            ((CSpringArm*)c->scs[boom].data)->target_arm_length = 0.0f;
            ((CSpringArm*)c->scs[boom].data)->do_collision_test = 0;
            c->scs[boom].location[2] = 64.0f;
        }
        c->comp_mask &= ~COMP_BIT(COMP_STATIC_MESH);
    }
    if (!first_person) camera_lag_on(c);
    dg_sensor(c, 60.0f, 110.0f);
    return c;
}

// --- The HUD: a Widget Blueprint (Unreal's UMG) at the top left: a title, then each line a bar (with its label) or a
// text; the player's UpdateHUD event (a looping 0.1 s timer from BeginPlay) writes the values into it ---
typedef struct DgHudLine { const char* widget; bool bar; const char* label; const char* var; float max; } DgHudLine;

static BpAsset* dg_hud(const char* folder, const char* name, const char* title, const DgHudLine* lines, int n, const float accent[3]) {
    BpAsset* wb = template_class(folder, name, "User Widget");
    if (!wb) return NULL;
    if (!wb->ui) wb->ui = calloc(1, sizeof(UiTree));
    if (!wb->ui) return NULL;
    UiTree* t = wb->ui;
    ui_tree_default(t);
    int panel = ui_tree_add(t, UIW_BORDER, 0);
    if (panel < 0) return NULL;
    snprintf(t->w[panel].name, UI_NAME, "Panel");
    t->w[panel].anchor[1] = t->w[panel].anchor[3] = 1.0f, t->w[panel].align[1] = 1.0f; // (bottom left: the messages print at the top)
    t->w[panel].pos[0] = 24, t->w[panel].pos[1] = -24, t->w[panel].size[0] = 360, t->w[panel].size[1] = 56.0f + 40.0f * (float)n, t->w[panel].padding = 12;
    t->w[panel].bg[0] = t->w[panel].bg[1] = t->w[panel].bg[2] = 0.04f, t->w[panel].bg[3] = 0.72f;
    int box = ui_tree_add(t, UIW_VBOX, panel);
    if (box < 0) return NULL;
    t->w[box].padding = 6;
    int ti = ui_tree_add(t, UIW_TEXT, box);
    if (ti >= 0) {
        snprintf(t->w[ti].name, UI_NAME, "Title");
        snprintf(t->w[ti].text, sizeof t->w[ti].text, "%s", title);
        t->w[ti].font_size = 26, t->w[ti].size[1] = 32;
        memcpy(t->w[ti].color, accent, 12), t->w[ti].color[3] = 1.0f;
    }
    for (int i = 0; i < n; i++) {
        int k = ui_tree_add(t, lines[i].bar ? UIW_PROGRESS : UIW_TEXT, box);
        if (k < 0) break;
        snprintf(t->w[k].name, UI_NAME, "%s", lines[i].widget);
        t->w[k].size[1] = lines[i].bar ? 24.0f : 30.0f;
        if (lines[i].bar) memcpy(t->w[k].color, accent, 12), t->w[k].color[3] = 1.0f, t->w[k].percent = 1.0f;
        else snprintf(t->w[k].text, sizeof t->w[k].text, "%s", lines[i].label), t->w[k].font_size = 22;
    }
    bp_save(wb);
    return wb;
}

// The player's side: BeginPlay (after `after`) creates the HUD, keeps it in the HUD variable and starts the timer; the
// UpdateHUD event sets each line: a bar's percent (var / max), a text ("label" + the var)
static void dg_hud_player(BpAsset* c, const BpAsset* hud, uint32_t after, const DgHudLine* lines, int n, float y0) {
    if (!hud) return;
    dg_var(c, "HUD", BPT_ACTOR, 0);
    uint32_t cw = dg_node(c, BPK_CREATE_WIDGET, 1200, y0, NULL), av = dg_node(c, BPK_ADD_TO_VIEWPORT, 1420, y0, NULL);
    uint32_t sh = dg_node(c, BPK_VAR_SET, 1640, y0, "HUD"), tm = dg_node(c, BPK_SET_TIMER, 1860, y0, "UpdateHUD");
    dg_text(c, cw, "Class", hud->name);
    dg_wire(c, after, "", cw, "");
    dg_wire(c, cw, "", av, "");
    dg_wire(c, cw, "Return Value", av, "Target");
    dg_wire(c, av, "", sh, "");
    dg_wire(c, cw, "Return Value", sh, "HUD");
    dg_wire(c, sh, "", tm, "");
    dg_num(c, tm, "Time", 0.1f);
    dg_num(c, tm, "Looping", 1.0f);
    uint32_t ev = dg_node(c, BPK_EVENT_CUSTOM, 1200, y0 + 300, "UpdateHUD");
    uint32_t prev = ev;
    for (int i = 0; i < n; i++) {
        float x = 1420.0f + (float)i * 260.0f, y = y0 + 300;
        uint32_t hg = dg_node(c, BPK_VAR_GET, x, y + 160, "HUD"), vg = dg_node(c, BPK_VAR_GET, x, y + 240, lines[i].var);
        uint32_t s;
        if (lines[i].bar) {
            s = dg_node(c, BPK_WIDGET_SET_PERCENT, x, y, lines[i].widget);
            uint32_t dv = dg_node(c, BPK_DIV, x + 120, y + 240, NULL);
            dg_wire(c, vg, lines[i].var, dv, "A");
            dg_num(c, dv, "B", lines[i].max);
            dg_wire(c, dv, "", s, "In Percent");
        } else {
            s = dg_node(c, BPK_WIDGET_SET_TEXT, x, y, lines[i].widget);
            uint32_t ap = dg_node(c, BPK_APPEND, x + 120, y + 240, NULL);
            dg_text(c, ap, "A", lines[i].label);
            dg_wire(c, vg, lines[i].var, ap, "B");
            dg_wire(c, ap, "Return Value", s, "In Text");
        }
        dg_wire(c, hg, "HUD", s, "Target");
        dg_wire(c, prev, "", s, "");
        prev = s;
    }
}

// --- RPG ---
// --- Horror ---
// A widget of a HUD: a text (or a bar) at an anchor of the screen (ax, ay: 0-1), its pivot (px, py)
static int dg_w(UiTree* t, int kind, const char* name, float ax, float ay, float px, float py, float x, float y, float w, float h,
                const char* text, float font, const float col[4]) {
    int k = ui_tree_add(t, kind, 0);
    if (k < 0) return -1;
    UiWidget* u = &t->w[k];
    snprintf(u->name, UI_NAME, "%s", name);
    u->anchor[0] = u->anchor[2] = ax, u->anchor[1] = u->anchor[3] = ay;
    u->align[0] = px, u->align[1] = py;
    u->pos[0] = x, u->pos[1] = y, u->size[0] = w, u->size[1] = h;
    if (text) snprintf(u->text, sizeof u->text, "%s", text);
    if (font > 0.0f) u->font_size = font;
    if (col) memcpy(u->color, col, 16);
    u->shadow = 1;
    return k;
}

static Entity dg_named(World* w, const char* name) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_NAME))) continue;
        const CName* n = ecs_arch_column(a, COMP_NAME);
        for (uint32_t i = 0; i < a->count; i++)
            if (!strcmp(n[i].text, name)) return ecs_arch_entities(a)[i];
    }
    return ENTITY_NONE;
}
// The demo's colored shapes in their colors: a new shape gets the grid material (the prototyping look), the demos paint
// theirs (ropes, awnings, crowns, fuses): the grid taken off every one but the floor
static void dg_plain_colors(World* w) {
    uint32_t grid = content_material("M_Grid.rmat");
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_STATIC_MESH))) continue;
        CStaticMesh* m = ecs_arch_column(a, COMP_STATIC_MESH);
        const CName* n = (a->mask & COMP_BIT(COMP_NAME)) ? ecs_arch_column(a, COMP_NAME) : NULL;
        for (uint32_t i = 0; i < a->count; i++)
            if (m[i].material == grid && !(n && !strcmp(n[i].text, "Floor"))) m[i].material = 0;
    }
}

static void dg_material(World* w, Entity e, uint32_t mat) {
    CStaticMesh* m = e && mat ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
    if (m) m->material = mat;
}

// A wall of the house from (x0, y0) to (x1, y1) (prototyping kit: collides, navigates), 320 cm high; door: its opening's
// offset from the wall's middle (NAN: none), 120 wide, 225 high
static Entity hz_wall(Editor* ed, float x0, float y0, float x1, float y1, float door, uint32_t mat) {
    float dx = x1 - x0, dy = y1 - y0, len = sqrtf(dx * dx + dy * dy);
    Entity e = dg_proto(ed, PROTO_WALL, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, len, 20, 320, atan2f(dy, dx) * 57.29578f, false,
                        isnan(door) ? 0.0f : 120.0f, (float[3]){ 0.3f, 0.28f, 0.26f });
    CProto* p = e ? ecs_get(&ed->world, e, COMP_PROTO) : NULL;
    if (p) {
        if (!isnan(door)) p->opening[1] = 225.0f, p->opening[2] = door;
        if (mat) p->material = mat;
    }
    return e;
}
// A floor (or the ceiling) slab over a rectangle
static void hz_slab(Editor* ed, float x0, float y0, float x1, float y1, float z, float thick, uint32_t mat) {
    Entity e = dg_proto(ed, PROTO_BOX, (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, x1 - x0, y1 - y0, thick, 0, false, 0, (float[3]){ 0.3f, 0.3f, 0.3f });
    CTransform* t = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
    if (t) t->location[2] = z;
    CProto* p = e ? ecs_get(&ed->world, e, COMP_PROTO) : NULL;
    if (p && mat) p->material = mat;
}
// Something the player uses (E): a named point (the demo's game mode finds it by its name)
static Entity hz_marker(Editor* ed, const char* name, float x, float y, float z) {
    World* w = &ed->world;
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    if (!e) return e;
    set_name(w, e, name);
    CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
    t->location[0] = x, t->location[1] = y, t->location[2] = z;
    t->scale[0] = t->scale[1] = t->scale[2] = 1.0f;
    t->flags = MOBILITY_STATIC;
    return e;
}
// A sheet of paper (a note) lying on something, faintly lit so it reads in the dark
static void hz_note(Editor* ed, const char* name, float x, float y, float z) {
    const float paper[3] = { 0.85f, 0.82f, 0.72f };
    Entity e = mesh_actor(ed, SHAPE_CUBE, x, y, z, paper, 0.0f, 0.8f);
    CTransform* t = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
    if (t) t->scale[0] = 0.21f, t->scale[1] = 0.3f, t->scale[2] = 0.01f, t->rotation[1] = 17.0f, t->flags = MOBILITY_STATIC;
    CStaticMesh* m = e ? ecs_get(&ed->world, e, COMP_STATIC_MESH) : NULL;
    if (m) m->emissive[0] = m->emissive[1] = 0.35f, m->emissive[2] = 0.25f;
    if (e) set_name(&ed->world, e, name);
}
static Entity hz_light(Editor* ed, const char* name, float x, float y, float z, float r, float g, float b, float intensity, float radius, bool shadows) {
    Entity e = dg_light(ed, x, y, z, r, g, b, intensity, radius);
    CLight* l = e ? ecs_get(&ed->world, e, COMP_LIGHT) : NULL;
    if (l) l->cast_shadows = shadows ? 1 : 0;
    if (e && name) set_name(&ed->world, e, name);
    return e;
}

// The horror HUD (WBP_HorrorHUD; demo_horror.inc writes into it): the objective and the fuses at the top left, the
// prompt (E) under the middle, the messages, a note's page in the middle, the banner
static void horror_hud_asset(void) {
    BpAsset* wb = template_class("/Content/Horror", "WBP_HorrorHUD", "User Widget");
    if (!wb) return;
    if (!wb->ui) wb->ui = calloc(1, sizeof(UiTree));
    if (!wb->ui) return;
    UiTree* t = wb->ui;
    ui_tree_default(t);
    const float pale[4] = { 0.9f, 0.86f, 0.78f, 1 }, dim[4] = { 0.75f, 0.72f, 0.66f, 0.9f }, red[4] = { 0.85f, 0.15f, 0.1f, 1 }, ink[4] = { 0.12f, 0.1f, 0.08f, 1 };
    dg_w(t, UIW_TEXT, "Objective", 0, 0, 0, 0, 40, 36, 1100, 34, "", 26, pale);
    dg_w(t, UIW_TEXT, "Items", 0, 0, 0, 0, 40, 74, 600, 30, "", 22, dim);
    int pr = dg_w(t, UIW_TEXT, "Prompt", 0.5f, 0.62f, 0.5f, 0.5f, 0, 0, 900, 34, "", 26, pale);
    if (pr >= 0) t->w[pr].visible = 0;
    int ms = dg_w(t, UIW_TEXT, "Message", 0.5f, 0.85f, 0.5f, 0.5f, 0, 0, 1700, 40, "", 26, pale);
    if (ms >= 0) t->w[ms].visible = 0;
    int nb = dg_w(t, UIW_BORDER, "NoteBack", 0.5f, 0.45f, 0.5f, 0.5f, 0, 0, 1000, 420, NULL, 0, NULL);
    if (nb >= 0) {
        t->w[nb].bg[0] = 0.86f, t->w[nb].bg[1] = 0.82f, t->w[nb].bg[2] = 0.7f, t->w[nb].bg[3] = 0.97f;
        t->w[nb].radius = 3, t->w[nb].visible = 0;
    }
    int nt = dg_w(t, UIW_TEXT, "NoteText", 0.5f, 0.45f, 0.5f, 0.5f, 0, 0, 900, 340, "", 28, ink);
    if (nt >= 0) t->w[nt].visible = 0, t->w[nt].wrap = 1, t->w[nt].shadow = 0;
    int bn = dg_w(t, UIW_TEXT, "Banner", 0.5f, 0.4f, 0.5f, 0.5f, 0, 0, 1400, 100, "", 80, red);
    if (bn >= 0) t->w[bn].visible = 0, t->w[bn].bold = 1;
    bp_save(wb);
}

static void demo_horror(Editor* ed) {
    ProjectSettings* ps = project();
    World* w = &ed->world;
    template_ground(ed, 80.0f);
    editor_sky_preset(ed, 6, 0.0); // night (moonlight)
    Entity fog = find_actor(w, COMP_FOG, 0);
    CFog* f = fog ? ecs_get(w, fog, COMP_FOG) : NULL;
    if (f) f->density = 0.012f, f->volumetric = 1, f->volumetric_distance = 2500.0f; // (dust in the flashlight's cone)
    ps->game_scalability = 1; // Medium: closed dark rooms make the full ray-traced GI cost ~2.5 ms (High ~160 fps, Medium ~300)
    Entity pp = spawn_actor(ed, SPAWN_POST, 0, (float[3]){ 0, 0, 0 }, 0.0); // the night stays dark; cold, a vignette, grain
    CPostProcess* pv = pp ? ecs_get(w, pp, COMP_POST_PROCESS) : NULL;
    if (pv) {
        pv->flags = PPV_ENABLED | PPV_UNBOUND;
        // (Q13) the horror exposure: metered by what is lit (the flashlight's spot, a lamp), never brighter than EV 1 (the
        // dark stays dark), the toe crushing the shadows; up to EV 4 when the power is back
        const int fs[] = { PP_EXPOSURE_MIN_EV, PP_EXPOSURE_MAX_EV, PP_VIGNETTE, PP_GRAIN, PP_SATURATION, PP_TEMPERATURE };
        const float vs[] = { 1.0f, 4.0f, 0.6f, 0.12f, 0.65f, 8500.0f };
        for (int k = 0; k < 6; k++) pv->overrides |= 1u << fs[k], pv->s.v[fs[k]] = vs[k];
        pv->terror_flags = TERROR_PPV_EXPOSURE, pv->terror_meter = 0.95f, pv->terror_toe = 0.45f, pv->terror_ev_floor = 1.0f;
    }
    kit_licenses();
    // the materials (ambientCG, World Aligned): old wallpaper, peeling plaster, worn floorboards, clinic tiles, concrete, rust
    uint32_t paper = kit_material("Horror", "Wallpaper002A", 160.0f, (float[3]){ 0.62f, 0.58f, 0.52f }, 1.0f);
    uint32_t plaster = kit_material("Horror", "PaintedPlaster017", 220.0f, (float[3]){ 0.75f, 0.78f, 0.74f }, 1.0f);
    uint32_t boards = kit_material("Horror", "WoodFloor051", 220.0f, (float[3]){ 0.7f, 0.62f, 0.55f }, 1.0f);
    uint32_t tiles = kit_material("Horror", "Tiles074", 120.0f, (float[3]){ 0.85f, 0.88f, 0.85f }, 1.0f);
    uint32_t concrete = kit_material("Horror", "Concrete034", 260.0f, NULL, 1.0f);
    uint32_t rust = kit_material("Horror", "Rust004", 150.0f, NULL, 1.0f);
    // the plan (cm): the foyer (you start here), the library north of it, the corridor east with the infirmary and the
    // storeroom south of it, the office and the generator room north of it, the gate at its end, the exit hall past it
    hz_slab(ed, -600, -500, 600, 1400, 0, 6, boards);      // foyer + library
    hz_slab(ed, 600, -150, 3000, 150, 0, 6, boards);       // corridor
    hz_slab(ed, 600, -1100, 1700, -150, 0, 6, tiles);      // infirmary
    hz_slab(ed, 1700, -900, 2500, -150, 0, 6, concrete);   // storeroom
    hz_slab(ed, 600, 150, 1600, 1000, 0, 6, boards);       // office
    hz_slab(ed, 1600, 150, 2600, 1100, 0, 6, concrete);    // generator room
    hz_slab(ed, 3000, -500, 3800, 500, 0, 6, concrete);    // exit hall
    hz_slab(ed, -620, -1120, 3820, 1420, 320, 12, plaster); // the ceiling over everything
    const float N = NAN;
    hz_wall(ed, -600, -500, 600, -500, N, paper), hz_wall(ed, -600, -500, -600, 1400, N, paper);     // foyer south, west
    hz_wall(ed, -600, 500, 600, 500, 0.0f, paper);                                                   // foyer | library
    hz_wall(ed, -600, 1400, 600, 1400, N, paper), hz_wall(ed, 600, 500, 600, 1400, N, paper);        // library north, east
    hz_wall(ed, 600, -500, 600, -150, N, paper), hz_wall(ed, 600, 150, 600, 500, N, paper);          // foyer east (the corridor's mouth)
    hz_wall(ed, 600, -150, 3000, -150, -650.0f, paper);                                              // corridor south: the infirmary's door
    hz_wall(ed, 600, 150, 3000, 150, -700.0f, paper);                                                // corridor north: the office's door
    hz_wall(ed, 1700, -150.01f, 2500, -150.01f, 0.0f, concrete);                                     // the storeroom's door (over it)
    hz_wall(ed, 1600, 150.01f, 2600, 150.01f, 0.0f, concrete);                                       // the generator room's door
    hz_wall(ed, 600, -1100, 600, -150, N, plaster), hz_wall(ed, 600, -1100, 1700, -1100, N, plaster); // infirmary west, south
    hz_wall(ed, 1700, -1100, 1700, -150, N, plaster);                                                // infirmary | storeroom
    hz_wall(ed, 1700, -900, 2500, -900, N, concrete), hz_wall(ed, 2500, -900, 2500, -150, N, concrete); // storeroom south, east
    hz_wall(ed, 600, 1000, 1600, 1000, N, paper), hz_wall(ed, 1600, 150, 1600, 1100, N, paper);      // office north, office | generator
    hz_wall(ed, 1600, 1100, 2600, 1100, N, concrete), hz_wall(ed, 2600, 150, 2600, 1100, N, concrete); // generator north, east
    hz_wall(ed, 3000, -500, 3000, 500, 0.0f, rust);                                                  // the gate wall
    hz_wall(ed, 3000, -500, 3800, -500, N, concrete), hz_wall(ed, 3000, 500, 3800, 500, N, concrete), hz_wall(ed, 3800, -500, 3800, 500, N, concrete);
    Entity gate = block(ed, 3000, 0, 112, 0.18f, 1.2f, 2.25f, 0, (float[3]){ 0.3f, 0.15f, 0.1f }); // the electric gate
    if (gate) set_name(w, gate, "ExitGate"), dg_material(w, gate, rust), ecs_add(w, gate, COMP_COLLISION);
    CCollision* gc = gate ? ecs_get(w, gate, COMP_COLLISION) : NULL;
    if (gc) gc->shape = COLLISION_BOX, gc->extent[0] = 9, gc->extent[1] = 60, gc->extent[2] = 112, gc->block = 1;
    // the furniture (Poly Haven): beds, a wheelchair and the medical box in the infirmary; shelves and the grandfather
    // clock in the library; cabinets; the generator; oil lamps that still burn
    KitProp bed = kit_prop(ed, "Horror", "old_bed_frame"), chair = kit_prop(ed, "Horror", "wheelchair_01"), med = kit_prop(ed, "Horror", "medical_box");
    for (int i = 0; i < 3; i++) kit_place(ed, &bed, 800.0f + (float)i * 320.0f, -900, 0, 90.0f, 1.0f, true);
    kit_place(ed, &chair, 1400, -400, 0, 200.0f, 1.0f, true), kit_place(ed, &chair, 2700, 60, 0, 30.0f, 1.0f, true);
    kit_place(ed, &med, 1550, -600, 0, 0.0f, 1.0f, true);
    kit_done(ed, &bed), kit_done(ed, &chair), kit_done(ed, &med);
    KitProp shelf = kit_prop(ed, "Horror", "wooden_bookshelf_worn"), clock = kit_prop(ed, "Horror", "vintage_grandfather_clock_01");
    for (int i = 0; i < 3; i++) kit_place(ed, &shelf, -400.0f + (float)i * 300.0f, 1330, 0, 180.0f, 1.0f, true);
    kit_place(ed, &shelf, 1000, 930, 0, 180.0f, 1.0f, true), kit_place(ed, &shelf, 2300, -830, 0, 0.0f, 1.0f, true);
    kit_place(ed, &clock, 520, 900, 0, 270.0f, 1.0f, true);
    kit_done(ed, &shelf), kit_done(ed, &clock);
    KitProp cab = kit_prop(ed, "Horror", "vintage_cabinet_01"), gen = kit_prop(ed, "Horror", "portable_generator");
    kit_place(ed, &cab, -520, 0, 0, 90.0f, 1.0f, true), kit_place(ed, &cab, 700, 700, 0, 90.0f, 1.0f, true), kit_place(ed, &cab, 1800, -400, 0, 90.0f, 1.0f, true);
    kit_place(ed, &gen, 2100, 750, 0, 0.0f, 1.3f, true);
    kit_done(ed, &cab), kit_done(ed, &gen);
    KitProp lamp = kit_prop(ed, "Horror", "vintage_oil_lamp");
    static const float lamps[4][3] = { { -520, 0, 95 }, { 700, 700, 95 }, { 1800, -400, 95 }, { 2900, 0, 0 } };
    for (int i = 0; i < 4; i++) {
        kit_place(ed, &lamp, lamps[i][0], lamps[i][1], lamps[i][2], 0.0f, 1.0f, false);
        Entity fl = hz_light(ed, NULL, lamps[i][0], lamps[i][1], lamps[i][2] + 30.0f, 1.0f, 0.55f, 0.25f, 18.0f, 500.0f, false); // (the flame)
        CLight* ll = fl ? ecs_get(w, fl, COMP_LIGHT) : NULL;
        if (ll) ll->flicker = FLICKER_CANDLE, ll->flicker_amount = 0.6f, ll->flicker_speed = 0.8f + 0.15f * (float)i; // (Q13: the flame sways)
    }
    kit_done(ed, &lamp);
    // what the player uses: the notes, the fuses (the third hidden in the clock's case), the clock, the generator, the
    // breakers on the generator room's wall, the way out
    hz_note(ed, "NoteFoyer", -480, -30, 98), hz_note(ed, "NoteLibrary", 0, 1250, 88), hz_note(ed, "NoteOffice", 820, 700, 98);
    const float fuse_col[3] = { 0.9f, 0.6f, 0.2f };
    Entity f1 = dg_pickup(ed, "Fuse1", SHAPE_CYLINDER, 1550, -600, 70, fuse_col, 2.0f, 60.0f);
    Entity f2 = dg_pickup(ed, "Fuse2", SHAPE_CYLINDER, 2350, -400, 15, fuse_col, 2.0f, 60.0f);
    Entity f3 = dg_pickup(ed, "Fuse3", SHAPE_CYLINDER, 470, 900, 60, fuse_col, 2.0f, 60.0f);
    Entity fuses[3] = { f1, f2, f3 };
    for (int i = 0; i < 3; i++) {
        CTransform* t = fuses[i] ? ecs_get(w, fuses[i], COMP_TRANSFORM) : NULL;
        if (t) t->scale[0] = t->scale[1] = 0.08f, t->scale[2] = 0.14f;
        if (t && i == 2) t->flags |= TRANSFORM_HIDDEN_GAME; // (until the clock opens)
    }
    hz_marker(ed, "Clock", 520, 900, 120), hz_marker(ed, "Generator", 2100, 750, 80), hz_marker(ed, "Exit", 3600, 0, 100);
    for (int i = 0; i < 4; i++) { // the breakers: red levers on a grey panel
        char nm[16];
        snprintf(nm, sizeof nm, "Breaker%d", i + 1);
        Entity b = mesh_actor(ed, SHAPE_CUBE, 2580, 400.0f + (float)i * 70.0f, 150, (float[3]){ 0.6f, 0.08f, 0.05f }, 0.6f, 0.4f);
        CTransform* t = b ? ecs_get(w, b, COMP_TRANSFORM) : NULL;
        if (t) t->scale[0] = 0.06f, t->scale[1] = 0.12f, t->scale[2] = 0.25f, t->flags = MOBILITY_STATIC;
        CStaticMesh* m = b ? ecs_get(w, b, COMP_STATIC_MESH) : NULL;
        if (m) m->emissive[0] = 0.5f;
        if (b) set_name(w, b, nm);
    }
    // the house's lights: dead until the power is back (their names: the game mode turns them on); two make shadows
    static const float rooms[8][2] = { { 0, 0 }, { 0, 950 }, { 1150, -620 }, { 2100, -520 }, { 1100, 580 }, { 2100, 620 }, { 1800, 0 }, { 3400, 0 } };
    for (int i = 0; i < 8; i++) {
        char nm[24];
        snprintf(nm, sizeof nm, "PowerLight%d", i + 1);
        Entity pl = hz_light(ed, nm, rooms[i][0], rooms[i][1], 290, 1.0f, 0.85f, 0.65f, 260.0f, 900.0f, i == 0 || i == 5);
        CLight* ll = pl ? ecs_get(w, pl, COMP_LIGHT) : NULL;
        if (ll) { // (Q13) old bulbs: the corridor's two fail by themselves, all of them stutter as the tension climbs
            ll->flicker_tension = 0.7f;
            if (i == 6 || i == 3) ll->flicker = FLICKER_FAULTY, ll->flicker_amount = 0.85f;
        }
    }
    Entity exit_l = hz_light(ed, NULL, 3600, 0, 280, 0.35f, 1.0f, 0.45f, 40.0f, 700.0f, false); // the exit's green light (on its own battery)
    CLight* el = exit_l ? ecs_get(w, exit_l, COMP_LIGHT) : NULL;
    if (el) el->flicker = FLICKER_PULSE, el->flicker_amount = 0.5f; // (Q13: a slow pulse, a beacon)
    // the flashlight (a spot the game mode keeps at your eyes); the thing that walks the house
    Entity fl = spawn_actor(ed, SPAWN_LIGHT, LIGHT_SPOT, (float[3]){ -300, -200, 160 }, 0.0);
    CLight* l = fl ? ecs_get(w, fl, COMP_LIGHT) : NULL;
    if (l) {
        l->color[0] = 1.0f, l->color[1] = 0.92f, l->color[2] = 0.78f, l->intensity = 900.0f;
        l->attenuation_radius = 2500.0f, l->inner_cone = 14.0f, l->outer_cone = 28.0f, l->cast_shadows = 1;
        l->beam_dust = 1; // (Q13: the dust drifting in its beam)
        ((CTransform*)ecs_get(w, fl, COMP_TRANSFORM))->flags = MOBILITY_MOVABLE;
        set_name(w, fl, "Flashlight");
    }
    Entity monster = dg_ai(ed, "Monster", 2200, 900, 180.0f, "Brain_Guard", "BT_Chase", 300.0f);
    // (Q13) what cuts the thing out of the dark: a cold Rim Light kept behind it as the camera sees it, on Lighting
    // Channel 1 (it lights only the creature: demo_horror.inc puts its parts on channels 0 and 1); its eyes shine back at
    // the flashlight (M_EyeGlow: the Eye shading model's Eye Glow)
    Entity rim = spawn_actor(ed, SPAWN_LIGHT, LIGHT_SPOT, (float[3]){ 2200, 1100, 250 }, 0.0);
    CLight* rl = rim ? ecs_get(w, rim, COMP_LIGHT) : NULL;
    if (rl) {
        rl->color[0] = 0.45f, rl->color[1] = 0.8f, rl->color[2] = 1.0f, rl->intensity = 1800.0f, rl->attenuation_radius = 900.0f;
        rl->inner_cone = 18.0f, rl->outer_cone = 32.0f, rl->cast_shadows = 0, rl->indirect_intensity = 0.0f;
        rl->rim = RIM_ACTOR, rl->rim_distance = 220.0f, rl->rim_height = 110.0f, rl->rim_side = 40.0f, rl->light_channels = 2u;
        snprintf(rl->rim_target, sizeof rl->rim_target, "Monster");
        ((CTransform*)ecs_get(w, rim, COMP_TRANSFORM))->flags = MOBILITY_MOVABLE;
        set_name(w, rim, "MonsterRim");
    }
    uint32_t eye_mesh_id = render_mesh_id_of("/Content/Meshes/Examples/SM_Eye.rmesh"), eye_mat = content_material("Examples/M_EyeGlow.rmat");
    for (int k = 0; monster && k < 2; k++) {
        Entity eye = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH) | COMP_BIT(COMP_ATTACH));
        if (!eye) break;
        set_name(w, eye, k ? "MonsterEyeR" : "MonsterEyeL");
        CTransform* et = ecs_get(w, eye, COMP_TRANSFORM);
        et->scale[0] = et->scale[1] = et->scale[2] = 0.034f, et->flags = MOBILITY_MOVABLE | TRANSFORM_NO_COLLISION;
        CStaticMesh* em = ecs_get(w, eye, COMP_STATIC_MESH);
        em->mesh = eye_mesh_id ? eye_mesh_id : SHAPE_SPHERE, em->material = eye_mesh_id ? eye_mat : 0u, em->roughness = 0.1f;
        em->base_color[0] = em->base_color[1] = em->base_color[2] = 0.05f, em->cast_shadow = CAST_SHADOW_NO_OCCLUDER;
        CAttach* at = ecs_get(w, eye, COMP_ATTACH);
        at->owner = at->parent = monster, at->kind = ATTACH_SCS;
        at->location[0] = 11.0f, at->location[1] = k ? 3.6f : -3.6f, at->location[2] = 72.0f;
        at->scale[0] = at->scale[1] = at->scale[2] = 0.034f;
    }
    BpAsset* c = dg_player(ed, "/Content/Horror", "BP_HorrorPlayer", true);
    if (!c) return;
    ((CCharacter*)c->comp_data[COMP_CHARACTER])->max_walk_speed = 380.0f;
    bp_save(c);
    horror_hud_asset();
    snprintf(ps->default_pawn, sizeof ps->default_pawn, "%s", c->name);
    snprintf(ps->game_mode, sizeof ps->game_mode, "DemoHorror"); // (the native game mode: demo_play.c)
    dg_plain_colors(w);
    bp_spawn(w, c, (float[3]){ -300, -200, 100 }, (float[3]){ 0, 0, 0 });
}

// The RPG's HUD (WBP_RPGHUD): the quest at the top left, the gold at the top right, the prompt, a toast, the dialogue
// box at the bottom (the speaker's name, the line); and the shop (WBP_Shop): the merchant's wares (a button each, a
// click buys), your bag (a click sells), your gold, Close. demo_rpg.inc writes into both.
static void rpg_ui_assets(void) {
    const float white[4] = { 1, 1, 1, 1 }, gold[4] = { 1.0f, 0.82f, 0.3f, 1 }, dim[4] = { 0.88f, 0.86f, 0.8f, 0.95f };
    BpAsset* wb = template_class("/Content/RPG", "WBP_RPGHUD", "User Widget");
    if (wb && (wb->ui || (wb->ui = calloc(1, sizeof(UiTree))))) {
        UiTree* t = wb->ui;
        ui_tree_default(t);
        int qb = dg_w(t, UIW_BORDER, "QuestBack", 0, 0, 0, 0, 28, 24, 760, 128, NULL, 0, NULL);
        if (qb >= 0) t->w[qb].bg[0] = t->w[qb].bg[1] = t->w[qb].bg[2] = 0.05f, t->w[qb].bg[3] = 0.6f, t->w[qb].radius = 6;
        dg_w(t, UIW_TEXT, "QuestTitle", 0, 0, 0, 0, 44, 32, 700, 32, "Quest: The Shield of Oakvale", 26, gold);
        dg_w(t, UIW_TEXT, "QuestStep", 0, 0, 0, 0, 44, 70, 700, 30, "", 22, white);
        dg_w(t, UIW_TEXT, "Jobs", 0, 0, 0, 0, 44, 106, 700, 28, "", 20, dim);
        int gb = dg_w(t, UIW_BORDER, "GoldBack", 1, 0, 1, 0, -28, 24, 240, 56, NULL, 0, NULL);
        if (gb >= 0) t->w[gb].bg[0] = t->w[gb].bg[1] = t->w[gb].bg[2] = 0.05f, t->w[gb].bg[3] = 0.6f, t->w[gb].radius = 6;
        dg_w(t, UIW_TEXT, "Gold", 1, 0, 1, 0, -48, 34, 200, 36, "50 gold", 30, gold);
        int pr = dg_w(t, UIW_TEXT, "Prompt", 0.5f, 0.66f, 0.5f, 0.5f, 0, 0, 900, 34, "", 26, white);
        if (pr >= 0) t->w[pr].visible = 0;
        int to = dg_w(t, UIW_TEXT, "Toast", 0.5f, 0, 0.5f, 0, 0, 120, 1200, 34, "", 24, gold);
        if (to >= 0) t->w[to].visible = 0;
        int db = dg_w(t, UIW_BORDER, "DialogBack", 0.5f, 1.0f, 0.5f, 1.0f, 0, -40, 1500, 200, NULL, 0, NULL);
        if (db >= 0) {
            t->w[db].bg[0] = 0.06f, t->w[db].bg[1] = 0.05f, t->w[db].bg[2] = 0.04f, t->w[db].bg[3] = 0.85f;
            t->w[db].radius = 8, t->w[db].border_w = 2, t->w[db].visible = 0;
            t->w[db].border_col[0] = 0.8f, t->w[db].border_col[1] = 0.65f, t->w[db].border_col[2] = 0.3f, t->w[db].border_col[3] = 1;
        }
        int dn = dg_w(t, UIW_TEXT, "DialogName", 0.5f, 1.0f, 0.5f, 1.0f, -560, -194, 360, 36, "", 28, gold);
        if (dn >= 0) t->w[dn].visible = 0, t->w[dn].bold = 1;
        int dt = dg_w(t, UIW_TEXT, "DialogText", 0.5f, 1.0f, 0.5f, 1.0f, 0, -100, 1420, 90, "", 26, white);
        if (dt >= 0) t->w[dt].visible = 0, t->w[dt].wrap = 1;
        bp_save(wb);
    }
    BpAsset* sb = template_class("/Content/RPG", "WBP_Shop", "User Widget");
    if (!sb || (!sb->ui && !(sb->ui = calloc(1, sizeof(UiTree))))) return;
    UiTree* t = sb->ui;
    ui_tree_default(t);
    int bg = dg_w(t, UIW_BORDER, "ShopBack", 0.5f, 0.5f, 0.5f, 0.5f, 0, 0, 1240, 860, NULL, 0, NULL);
    if (bg >= 0) {
        t->w[bg].bg[0] = 0.09f, t->w[bg].bg[1] = 0.07f, t->w[bg].bg[2] = 0.05f, t->w[bg].bg[3] = 0.94f, t->w[bg].radius = 10, t->w[bg].border_w = 2;
        t->w[bg].border_col[0] = 0.8f, t->w[bg].border_col[1] = 0.62f, t->w[bg].border_col[2] = 0.28f, t->w[bg].border_col[3] = 1;
    }
    dg_w(t, UIW_TEXT, "ShopTitle", 0.5f, 0.5f, 0.5f, 0.5f, 0, -390, 1100, 44, "", 34, gold);
    dg_w(t, UIW_TEXT, "WaresHead", 0.5f, 0.5f, 0, 0.5f, -590, -335, 560, 30, "WARES  (click to buy)", 20, dim);
    dg_w(t, UIW_TEXT, "BagHead", 0.5f, 0.5f, 0, 0.5f, 30, -335, 560, 30, "YOUR BAG  (click to sell, half price)", 20, dim);
    for (int r = 0; r < 20; r++) {
        char nm[16];
        snprintf(nm, sizeof nm, "Buy%d", r);
        int b = dg_w(t, UIW_BUTTON, nm, 0.5f, 0.5f, 0, 0.5f, -590, -298.0f + (float)r * 31.0f, 560, 28, "", 19, white);
        if (b >= 0) t->w[b].bg[0] = 0.2f, t->w[b].bg[1] = 0.16f, t->w[b].bg[2] = 0.11f, t->w[b].bg[3] = 1, t->w[b].radius = 4;
    }
    for (int r = 0; r < 14; r++) {
        char nm[16];
        snprintf(nm, sizeof nm, "Sell%d", r);
        int b = dg_w(t, UIW_BUTTON, nm, 0.5f, 0.5f, 0, 0.5f, 30, -298.0f + (float)r * 31.0f, 560, 28, "", 19, white);
        if (b >= 0) t->w[b].bg[0] = 0.13f, t->w[b].bg[1] = 0.17f, t->w[b].bg[2] = 0.12f, t->w[b].bg[3] = 1, t->w[b].radius = 4;
    }
    dg_w(t, UIW_TEXT, "BagEmpty", 0.5f, 0.5f, 0, 0.5f, 30, -298, 560, 28, "(empty)", 19, dim);
    dg_w(t, UIW_TEXT, "ShopGold", 0.5f, 0.5f, 0, 0.5f, -590, 380, 500, 36, "", 28, gold);
    int cl = dg_w(t, UIW_BUTTON, "Close", 0.5f, 0.5f, 1, 0.5f, 590, 380, 220, 40, "Close  (E)", 22, white);
    if (cl >= 0) t->w[cl].bg[0] = 0.35f, t->w[cl].bg[1] = 0.12f, t->w[cl].bg[2] = 0.08f, t->w[cl].bg[3] = 1, t->w[cl].radius = 5;
    bp_save(sb);
}

// A person of the village: a Mannequin standing, named (the game mode talks to it), facing yaw
static Entity rpg_person(Editor* ed, const char* name, float x, float y, float yaw) {
    Entity e = dg_ai(ed, name, x, y, yaw, NULL, NULL, 0.0f);
    if (e) ecs_remove(&ed->world, e, COMP_AI_AGENT); // (they stand and talk: no mind to run)
    return e;
}

// A house: brick walls (a room of the kit, its door to the street), a pitched tiled roof
static void rpg_house(Editor* ed, float x, float y, float yaw, uint32_t brick, uint32_t roof) {
    World* w = &ed->world;
    Entity h = dg_proto(ed, PROTO_ROOM, x, y, 700, 600, 320, yaw, true, 140, (float[3]){ 0.7f, 0.6f, 0.5f });
    CProto* p = h ? ecs_get(w, h, COMP_PROTO) : NULL;
    if (p && brick) p->material = brick;
    float c = cosf(yaw * 0.017453f), s = sinf(yaw * 0.017453f);
    for (int k = 0; k < 2; k++) { // the two slopes, over the long walls, meeting at the ridge
        float ly = k ? 160.0f : -160.0f;
        Entity r = block(ed, x - ly * s, y + ly * c, 395, 7.6f, 3.6f, 0.18f, 0, (float[3]){ 0.5f, 0.2f, 0.14f });
        CTransform* t = r ? ecs_get(w, r, COMP_TRANSFORM) : NULL;
        if (t) t->rotation[1] = yaw, t->rotation[2] = k ? 32.0f : -32.0f; // (each slope down toward its wall)
        dg_material(w, r, roof);
    }
}

// A market stall: a counter, posts, a striped awning, a table of wares beside it; its merchant behind the counter
static void rpg_stall(Editor* ed, float x, float y, float yaw, const float cloth[3], uint32_t planks, KitProp* table, KitProp* basket,
                      KitProp* crate, const char* merchant) {
    World* w = &ed->world;
    float c = cosf(yaw * 0.017453f), s = sinf(yaw * 0.017453f);
    Entity counter = block(ed, x, y, 55, 3.0f, 0.8f, 1.1f, 0, (float[3]){ 0.45f, 0.3f, 0.18f });
    CTransform* ct = counter ? ecs_get(w, counter, COMP_TRANSFORM) : NULL;
    if (ct) ct->rotation[1] = yaw;
    dg_material(w, counter, planks);
    Entity aw = block(ed, x - 60.0f * s, y + 60.0f * c, 265, 3.4f, 2.2f, 0.06f, 0, cloth);
    CTransform* at = aw ? ecs_get(w, aw, COMP_TRANSFORM) : NULL;
    if (at) at->rotation[1] = yaw, at->rotation[2] = 8.0f;
    for (int k = 0; k < 4; k++) {
        float lx = (k & 1) ? 160.0f : -160.0f, ly = (k & 2) ? 160.0f : -40.0f;
        Entity post = block(ed, x + lx * c - ly * s, y + lx * s + ly * c, 130, 0.1f, 0.1f, 2.6f, 0, (float[3]){ 0.4f, 0.27f, 0.15f });
        dg_material(w, post, planks);
    }
    kit_place(ed, table, x + 230.0f * c, y + 230.0f * s, 0, yaw, 1.0f, true);
    kit_place(ed, basket, x + 230.0f * c, y + 230.0f * s, 78, yaw + 20.0f, 0.8f, false);
    kit_place(ed, crate, x - 240.0f * c + 40.0f * s, y - 240.0f * s - 40.0f * c, 0, yaw + 12.0f, 1.0f, true);
    rpg_person(ed, merchant, x - 110.0f * s, y + 110.0f * c, yaw - 90.0f);
}

static void demo_rpg(Editor* ed) {
    ProjectSettings* ps = project();
    World* w = &ed->world;
    template_ground(ed, 120.0f);
    editor_sky_preset(ed, 1, 0.0); // morning
    kit_licenses();
    uint32_t grass = kit_material("RPG", "Grass004", 400.0f, (float[3]){ 0.85f, 0.95f, 0.8f }, 3.0f); // (its map averages 0.27: a lawn of it read as wet from afar; x3: ~0.8, a field)
    uint32_t stone = kit_material("RPG", "PavingStones070", 250.0f, NULL, 1.0f);
    uint32_t dirt = kit_material("RPG", "Ground037", 300.0f, NULL, 1.0f);
    uint32_t brick = kit_material("RPG", "Bricks105", 220.0f, NULL, 1.0f);
    uint32_t planks = kit_material("RPG", "Planks023A", 200.0f, NULL, 1.0f);
    uint32_t tiles = kit_material("RPG", "RoofingTiles012A", 180.0f, NULL, 1.0f);
    uint32_t bark = kit_material("RPG", "Bark007", 120.0f, NULL, 1.0f);
    dg_material(w, dg_named(w, "Floor"), grass);
    // the streets and the square (paving stones a little over the grass), the farm's field and the camp (earth)
    hz_slab(ed, -3200, -200, 3200, 200, 0, 3, stone);
    hz_slab(ed, -200, -2600, 200, 2600, 0, 3, stone);
    hz_slab(ed, -750, -750, 750, 750, 0, 3.5f, stone);
    hz_slab(ed, 1700, -2500, 3100, -1100, 0, 2, dirt);
    hz_slab(ed, -3100, -2500, -1700, -1300, 0, 2, dirt);
    // the houses along the streets, their doors to them; the Elder's (bigger) north of the square
    static const float houses[8][3] = { { -1500, 900, 180 }, { -2500, 900, 180 }, { 1300, 900, 180 }, { 2400, 900, 180 },
                                        { -1500, -950, 0 }, { -2500, -950, 0 }, { 1300, -950, 0 }, { -900, 2000, 90 } };
    for (int i = 0; i < 8; i++) rpg_house(ed, houses[i][0], houses[i][1], houses[i][2], brick, tiles);
    rpg_house(ed, 0, 1500, 180, brick, tiles); // the Elder's hall
    // the square: the fire pit in its middle, the four stalls round it
    KitProp table = kit_prop(ed, "RPG", "WoodenTable_01"), basket = kit_prop(ed, "RPG", "wicker_basket_01"), crate = kit_prop(ed, "RPG", "wooden_crate_01");
    const float red[3] = { 0.7f, 0.15f, 0.12f }, blue[3] = { 0.15f, 0.3f, 0.65f }, green[3] = { 0.2f, 0.5f, 0.25f }, ochre[3] = { 0.75f, 0.55f, 0.15f };
    rpg_stall(ed, -480, 480, 180, red, planks, &table, &basket, &crate, "Smith");
    rpg_stall(ed, 480, 480, 180, ochre, planks, &table, &basket, &crate, "Grocer");
    rpg_stall(ed, -480, -480, 0, green, planks, &table, &basket, &crate, "Alchemist");
    rpg_stall(ed, 480, -480, 0, blue, planks, &table, &basket, &crate, "Trader");
    KitProp pit = kit_prop(ed, "RPG", "stone_fire_pit"), barrels = kit_prop(ed, "RPG", "wooden_barrels_01"), chest = kit_prop(ed, "RPG", "treasure_chest");
    kit_place(ed, &pit, 0, 0, 3, 0, 1.2f, true);
    Entity fire = dg_light(ed, 0, 0, 60, 1.0f, 0.55f, 0.2f, 120.0f, 600.0f);
    CLight* fl = fire ? ecs_get(w, fire, COMP_LIGHT) : NULL;
    if (fl) fl->cast_shadows = 0;
    for (int i = 0; i < 8; i++) kit_place(ed, &barrels, houses[i][0] + 380.0f, houses[i][1] + (houses[i][2] > 90 ? -200.0f : 200.0f), 0, (float)(i * 40), 1.0f, true);
    kit_place(ed, &chest, 150, 1500, 0, 180, 1.0f, true);
    KitProp chair = kit_prop(ed, "RPG", "WoodenChair_01"), bucket = kit_prop(ed, "RPG", "wooden_bucket_01"), lamp = kit_prop(ed, "RPG", "street_lamp_01");
    kit_place(ed, &table, 1300, 420, 0, 0, 1.0f, true), kit_place(ed, &chair, 1220, 420, 0, 90, 1.0f, false), kit_place(ed, &chair, 1380, 420, 0, 270, 1.0f, false);
    kit_place(ed, &bucket, -1150, -500, 0, 0, 1.0f, true), kit_place(ed, &bucket, 2000, -1150, 0, 30, 1.0f, true);
    for (int i = -3; i <= 3; i++)
        if (i) kit_place(ed, &lamp, (float)i * 850.0f, 290.0f, 0, 0, 1.0f, true);
    kit_done(ed, &table), kit_done(ed, &basket), kit_done(ed, &crate), kit_done(ed, &pit), kit_done(ed, &barrels), kit_done(ed, &chest);
    kit_done(ed, &chair), kit_done(ed, &bucket), kit_done(ed, &lamp);
    // the lumber camp (south-west): dead trunks to chop, stumps, the lumberjack
    KitProp trunk = kit_prop(ed, "RPG", "dead_tree_trunk"), stump = kit_prop(ed, "RPG", "tree_stump_01");
    for (int i = 0; i < 5; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "Trunk%d", i + 1);
        Entity t = kit_place(ed, &trunk, -2900.0f + (float)(i % 3) * 450.0f, -2300.0f + (float)(i / 3) * 500.0f, 0, (float)(i * 63), 1.0f, false);
        if (t) set_name(w, t, nm);
        kit_place(ed, &stump, -2700.0f + (float)(i % 3) * 420.0f, -1550.0f - (float)(i / 3) * 320.0f, 0, (float)(i * 51), 1.0f, true);
    }
    kit_done(ed, &trunk), kit_done(ed, &stump);
    // the farm (south-east): rows of wheat sheaves
    for (int i = 0; i < 8; i++) {
        char nm[16];
        snprintf(nm, sizeof nm, "Wheat%d", i + 1);
        Entity s = mesh_actor(ed, SHAPE_CYLINDER, 1950.0f + (float)(i % 4) * 300.0f, -1450.0f - (float)(i / 4) * 450.0f, 55,
                              (float[3]){ 0.85f, 0.7f, 0.3f }, 0.0f, 0.85f);
        CTransform* t = s ? ecs_get(w, s, COMP_TRANSFORM) : NULL;
        if (t) t->scale[0] = t->scale[1] = 0.35f, t->scale[2] = 1.1f, t->flags = MOBILITY_STATIC;
        if (s) set_name(w, s, nm);
    }
    // trees round the village: bark trunks, rounded crowns
    for (int i = 0; i < 26; i++) {
        float a = (float)i * 0.2417f * 6.2832f, r = 3600.0f + (float)((i * 37) % 7) * 120.0f;
        float x = cosf(a) * r, y = sinf(a) * r * 0.8f;
        Entity tr = mesh_actor(ed, SHAPE_CYLINDER, x, y, 170, (float[3]){ 0.4f, 0.3f, 0.2f }, 0, 0.85f);
        CTransform* t = tr ? ecs_get(w, tr, COMP_TRANSFORM) : NULL;
        if (t) t->scale[0] = t->scale[1] = 0.45f, t->scale[2] = 3.4f, t->flags = MOBILITY_STATIC;
        dg_material(w, tr, bark);
        Entity cr = mesh_actor(ed, SHAPE_SPHERE, x, y, 470, (float[3]){ 0.2f + 0.05f * (float)(i % 3), 0.42f, 0.15f }, 0, 0.9f);
        t = cr ? ecs_get(w, cr, COMP_TRANSFORM) : NULL;
        if (t) t->scale[0] = t->scale[1] = 3.0f + 0.4f * (float)(i % 4), t->scale[2] = 3.4f, t->flags = MOBILITY_STATIC;
    }
    // the people
    rpg_person(ed, "Elder", 0, 420, 270);
    rpg_person(ed, "Miller", -2500, 520, 270);
    rpg_person(ed, "Lumberjack", -2300, -1400, 90);
    rpg_person(ed, "Farmer", 1900, -1100, 270);
    rpg_person(ed, "Courier", 850, -250, 180);
    rpg_person(ed, "Villager1", -1500, 520, 270), rpg_person(ed, "Villager2", 1300, 520, 270);
    rpg_person(ed, "Villager3", -1500, -570, 90), rpg_person(ed, "Villager4", 2400, 520, 270);
    rpg_ui_assets();
    BpAsset* c = dg_player(ed, "/Content/RPG", "BP_Adventurer", false);
    if (!c) return;
    bp_save(c);
    snprintf(ps->default_pawn, sizeof ps->default_pawn, "%s", c->name);
    snprintf(ps->game_mode, sizeof ps->game_mode, "DemoRPG"); // (the native game mode: demo_play.c)
    dg_plain_colors(w);
    bp_spawn(w, c, (float[3]){ 0, -1200, 100 }, (float[3]){ 0, 90, 0 });
}

// --- Fighting ---
// The fight's HUD (Widget Blueprint WBP_FightHUD; demo_fight.inc writes into it): the two health bars at the top with
// the names and the rounds won, the clock between them, the round, the combo counter, the banner, the controls
static void fight_hud_asset(void) {
    BpAsset* wb = template_class("/Content/Fighting", "WBP_FightHUD", "User Widget");
    if (!wb) return;
    if (!wb->ui) wb->ui = calloc(1, sizeof(UiTree));
    if (!wb->ui) return;
    UiTree* t = wb->ui;
    ui_tree_default(t);
    const float white[4] = { 1, 1, 1, 1 }, gold[4] = { 1.0f, 0.82f, 0.25f, 1 }, p1[4] = { 0.25f, 0.6f, 1.0f, 1 }, p2[4] = { 1.0f, 0.3f, 0.2f, 1 };
    const float dim[4] = { 0.85f, 0.85f, 0.85f, 0.85f };
    for (int s = 0; s < 2; s++) {
        float ax = s ? 1.0f : 0.0f, px = s ? 1.0f : 0.0f, dir = s ? -1.0f : 1.0f;
        int back = dg_w(t, UIW_BORDER, s ? "P2Back" : "P1Back", ax, 0, px, 0, 36 * dir, 34, 760, 44, NULL, 0, NULL);
        if (back >= 0) t->w[back].bg[0] = t->w[back].bg[1] = t->w[back].bg[2] = 0.03f, t->w[back].bg[3] = 0.75f, t->w[back].radius = 4;
        int bar = dg_w(t, UIW_PROGRESS, s ? "P2Bar" : "P1Bar", ax, 0, px, 0, 42 * dir, 40, 748, 32, NULL, 0, s ? p2 : p1);
        if (bar >= 0) t->w[bar].percent = 1.0f, t->w[bar].bg[3] = 0.6f, t->w[bar].radius = 3;
        dg_w(t, UIW_TEXT, s ? "P2Name" : "P1Name", ax, 0, px, 0, 42 * dir, 82, 400, 34, s ? "RIVAL" : "YOU", 26, white);
        dg_w(t, UIW_TEXT, s ? "P2Wins" : "P1Wins", ax, 0, px, 0, 240 * dir, 82, 300, 34, "", 24, gold);
    }
    dg_w(t, UIW_TEXT, "Timer", 0.5f, 0, 0.5f, 0, 0, 22, 140, 64, "60", 56, white);
    dg_w(t, UIW_TEXT, "Round", 0.5f, 0, 0.5f, 0, 0, 88, 300, 30, "ROUND 1", 24, gold);
    int combo = dg_w(t, UIW_TEXT, "Combo", 0, 0.4f, 0, 0.5f, 60, 0, 520, 60, "", 44, gold);
    if (combo >= 0) t->w[combo].visible = 0, t->w[combo].bold = 1;
    int ban = dg_w(t, UIW_TEXT, "Banner", 0.5f, 0.42f, 0.5f, 0.5f, 0, 0, 1400, 100, "", 72, white);
    if (ban >= 0) t->w[ban].visible = 0, t->w[ban].bold = 1;
    dg_w(t, UIW_TEXT, "Hint", 0.5f, 1.0f, 0.5f, 1.0f, 0, -18, 1800, 30,
         "WASD move   LMB jab > cross > hook > uppercut   RMB kick   Space jump (+LMB jump kick)   Shift block   Q sway   Ctrl roll", 20, dim);
    bp_save(wb);
}

static void demo_fighting(Editor* ed) {
    ProjectSettings* ps = project();
    World* w = &ed->world;
    template_ground(ed, 60.0f);
    editor_sky_preset(ed, 3, 0.0); // night: the club is lit by its own lamps
    anim_make_fight_pack(); // the guard, the strikes, the block, the dodge, the roll, the K.O. (anim_fight.inc)
    kit_licenses();
    // the materials (ambientCG, World Aligned): the concrete floor, the steel walls, the canvas of the ring
    uint32_t concrete = kit_material("Fighting", "Concrete040", 300.0f, NULL, 1.0f);
    uint32_t steel = kit_material("Fighting", "MetalPlates006", 250.0f, (float[3]){ 0.75f, 0.75f, 0.8f }, 1.0f);
    uint32_t canvas = kit_material("Fighting", "Fabric026", 120.0f, (float[3]){ 0.55f, 0.12f, 0.1f }, 1.0f);
    const float mat[3] = { 0.5f, 0.12f, 0.1f }, post[3] = { 0.85f, 0.85f, 0.88f }, rope[3] = { 0.9f, 0.1f, 0.1f }, wall[3] = { 0.35f, 0.36f, 0.4f };
    // the club: an underground hall 28 x 28 m, 9 m high (a room of the prototyping kit, its walls steel)
    Entity hall = dg_proto(ed, PROTO_ROOM, 0, 0, 2800, 2800, 900, 0, true, 0, wall);
    CProto* hp = hall ? ecs_get(w, hall, COMP_PROTO) : NULL;
    if (hp && steel) hp->material = steel;
    dg_material(w, dg_named(w, "Floor"), concrete);
    // the ring: 12 x 12 m, 80 cm high, canvas over it, steel posts, red ropes
    dg_material(w, block(ed, 0, 0, 40, 12, 12, 0.8f, 0, mat), canvas);
    for (int k = 0; k < 4; k++) {
        float x = (k & 1) ? 580.0f : -580.0f, y = (k & 2) ? 580.0f : -580.0f;
        dg_material(w, block(ed, x, y, 160, 0.3f, 0.3f, 2.4f, 0, post), steel);
    }
    for (int s = 0; s < 4; s++)
        for (int h = 0; h < 3; h++) {
            float z = 130.0f + (float)h * 45.0f;
            Entity r = s < 2 ? block(ed, 0, s ? 580.0f : -580.0f, z, 11.6f, 0.05f, 0.05f, 0, rope)
                             : block(ed, s == 3 ? 580.0f : -580.0f, 0, z, 0.05f, 11.6f, 0.05f, 0, rope);
            CStaticMesh* rm = r ? ecs_get(w, r, COMP_STATIC_MESH) : NULL;
            if (rm) rm->emissive[0] = 0.4f; // (a little glow: the ropes read in the dark)
        }
    // the props (Poly Haven): lamps over the ring, stoves burning in the corners, crates and tool chests along the walls,
    // searchlights on the fight. (The chain-link fence was 26 parts a module: 700 draws for a backdrop; left out)
    KitProp lamp = kit_prop(ed, "Fighting", "hanging_industrial_lamp");
    for (int k = 0; k < 4; k++) {
        float x = (k & 1) ? 300.0f : -300.0f, y = (k & 2) ? 300.0f : -300.0f;
        kit_place(ed, &lamp, x, y, 640, 0, 1.4f, false);
        Entity l = dg_light(ed, x, y, 600, 1.0f, 0.86f, 0.68f, 9000.0f, 1500.0f);
        CLight* cl = l ? ecs_get(w, l, COMP_LIGHT) : NULL;
        if (cl && k >= 2) cl->cast_shadows = 0; // (two lamps shadow the fighters; the rest only light: ray traced shadows cost)
    }
    kit_done(ed, &lamp);
    KitProp stove = kit_prop(ed, "Fighting", "barrel_stove");
    static const float corners[4][2] = { { -1150, -1150 }, { 1150, -1150 }, { -1150, 1150 }, { 1150, 1150 } };
    for (int k = 0; k < 4; k++) {
        kit_place(ed, &stove, corners[k][0], corners[k][1], 0, 45.0f * (float)k, 1.2f, true);
        Entity l = dg_light(ed, corners[k][0], corners[k][1], 90, 1.0f, 0.42f, 0.1f, 450.0f, 700.0f); // (the fire in it: a glow, not a lamp)
        CLight* cl = l ? ecs_get(w, l, COMP_LIGHT) : NULL;
        if (cl) cl->cast_shadows = 0;
    }
    kit_done(ed, &stove);
    KitProp crate = kit_prop(ed, "Fighting", "wooden_military_crate"), chest = kit_prop(ed, "Fighting", "metal_tool_chest");
    for (int i = 0; i < 6; i++) {
        float y = -900.0f + (float)i * 360.0f;
        kit_place(ed, &crate, -1300, y, 0, (float)(i * 37 % 40) - 20.0f, 1.0f, true);
        if (i % 2 == 0) kit_place(ed, &crate, -1300, y + 40.0f, 60, 10.0f, 1.0f, false);
        kit_place(ed, &chest, 1300, y, 0, 180.0f + (float)(i * 23 % 30) - 15.0f, 1.0f, true);
    }
    kit_done(ed, &crate), kit_done(ed, &chest);
    Entity fill = dg_light(ed, 0, 0, 820, 0.7f, 0.8f, 1.0f, 3000.0f, 2400.0f); // (the hall's fill)
    CLight* fl = fill ? ecs_get(w, fill, COMP_LIGHT) : NULL;
    if (fl) fl->cast_shadows = 0;
    // indoors: the night preset's clouds go, its fog thins to a smoky haze that the lamps light
    ecs_destroy(w, dg_named(w, "VolumetricCloud"));
    CFog* fog = ecs_get(w, dg_named(w, "ExponentialHeightFog"), COMP_FOG);
    if (fog) fog->density = 0.0015f, fog->height_falloff = 0.05f;
    // the rival: a Mannequin with ABP_Fighter; its mind is the game mode's (demo_fight.inc)
    Entity rival = dg_ai(ed, "Rival", 230, 0, 180.0f, NULL, NULL, 330.0f);
    const BpAsset* abp = bp_find_name(BPA_CLASS, "ABP_Fighter");
    CTransform* rt = rival ? ecs_get(w, rival, COMP_TRANSFORM) : NULL;
    if (rt) rt->location[2] = 80.0f + 92.0f + 2.0f; // (on the ring)
    CSkeletalMesh* rsk = rival ? ecs_get(w, rival, COMP_SKELETAL_MESH) : NULL;
    if (rsk && abp) rsk->anim_class = abp->hash;
    if (rival) ecs_remove(w, rival, COMP_AI_AGENT); // (its mind is the game mode's: the AI system would take its input)
    BpAsset* c = dg_player(ed, "/Content/Fighting", "BP_Fighter", false);
    if (!c) return;
    int mesh = scs_index(c, "Mesh");
    if (mesh >= 0 && abp) ((CSkeletalMesh*)c->scs[mesh].data)->anim_class = abp->hash;
    ((CCharacter*)c->comp_data[COMP_CHARACTER])->max_walk_speed = 330.0f; // (the footwork's pace)
    bp_save(c);
    fight_hud_asset();
    snprintf(ps->default_pawn, sizeof ps->default_pawn, "%s", c->name);
    snprintf(ps->game_mode, sizeof ps->game_mode, "DemoFighting"); // (the native game mode: demo_play.c)
    dg_plain_colors(w);
    bp_spawn(w, c, (float[3]){ -230, 0, 180 }, (float[3]){ 0, 0, 0 });
}
