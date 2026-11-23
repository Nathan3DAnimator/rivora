// A vehicle from the user's own meshes (vehicle.h vehicle_from_actors; included by vehicle.c). 2026-11-08, the user:
// "the vehicle system is confusing when one makes a vehicle from a mesh one imported: make it more dynamic". Select the
// model's pieces in the level, Actor > Make Vehicle from Selected > a kind: the body, the wheels and the doors are found
// and measured, the template's physics fitted to them, and the vehicle drives at once.

typedef struct BuildPiece {
    Entity e;
    uint32_t mesh, material;
    float mn[3], mx[3];   // its bounds in the vehicle's frame (cm, from the body's place, its front along +X)
    float rot[3], scale[3];
    float loc[3];         // its place in that frame
    int role;             // 0 body, 1 wheel, 2 door, 3 rides on the body
    char name[RE_NAME_MAX];
} BuildPiece;

static bool name_has(const char* name, const char* const* words) {
    char low[RE_NAME_MAX];
    size_t n = strlen(name);
    if (n >= sizeof low) n = sizeof low - 1;
    for (size_t i = 0; i < n; i++) low[i] = (char)tolower((unsigned char)name[i]);
    low[n] = 0;
    for (int i = 0; words[i]; i++)
        if (strstr(low, words[i])) return true;
    return false;
}

static const char* const k_wheel_words[] = { "wheel", "tire", "tyre", "roda", "pneu", "rueda", "llanta", "neum", "rim", NULL };
static const char* const k_door_words[] = { "door", "porta", "puerta", NULL };

// The world bounds of a Static Mesh actor (its 8 corners through its transform)
static void piece_world_bounds(const CTransform* t, uint32_t mesh, float mn[3], float mx[3]) {
    float lmn[3], lmx[3], rows[3][4];
    render_mesh_bounds(mesh, lmn, lmx);
    render_transform_rows(t, rows);
    for (int k = 0; k < 3; k++) mn[k] = 1e30f, mx[k] = -1e30f;
    for (int c = 0; c < 8; c++) {
        float p[3] = { c & 1 ? lmx[0] : lmn[0], c & 2 ? lmx[1] : lmn[1], c & 4 ? lmx[2] : lmn[2] };
        for (int k = 0; k < 3; k++) {
            float v = rows[k][0] * p[0] + rows[k][1] * p[1] + rows[k][2] * p[2] + rows[k][3];
            mn[k] = fminf(mn[k], v), mx[k] = fmaxf(mx[k], v);
        }
    }
}

// The template's first wheel at the front (+x) and at the back: their suspension, tire, steering, drive and brakes
static void template_wheels(const VehicleAsset* t, VehPart* front, VehPart* back) {
    bool f = false, b = false;
    for (uint32_t i = 0; i < t->nparts; i++) {
        const VehPart* p = &t->parts[i];
        if (p->kind != VP_WHEEL) continue;
        if (p->pos[0] >= 0 && !f) *front = *p, f = true;
        if (p->pos[0] < 0 && !b) *back = *p, b = true;
    }
    if (!f && b) *front = *back;
    if (!b && f) *back = *front;
}

static void set_wheel(VehPart* p, const VehPart* tw, const char* name, const float c[3], float radius, float width) {
    *p = *tw;
    snprintf(p->name, VEH_NAME, "%s", name), snprintf(p->attach, VEH_NAME, "%s", name);
    p->p[0] = fminf(fmaxf(radius, 5.0f), 300.0f), p->p[1] = fminf(fmaxf(width, 2.0f), 200.0f);
    // the mount: above the wheel's center by its hang at rest (vehicle_spawn hangs it 0.6 of its travel)
    p->pos[0] = c[0], p->pos[1] = c[1], p->pos[2] = c[2] + p->p[4] * 0.6f;
    p->mesh = p->material = 0;
    memset(p->mrot, 0, sizeof p->mrot);
    p->mscale[0] = p->mscale[1] = p->mscale[2] = 1.0f;
}

// Wheels under the body's corners (no wheel meshes): 4 (2 on a motorcycle, 6 a side on a tank), invisible
static void place_wheels(VehicleAsset* a, const VehicleAsset* tmpl, const float mn[3], const float mx[3]) {
    VehPart tf, tb;
    memset(&tf, 0, sizeof tf), memset(&tb, 0, sizeof tb);
    template_wheels(tmpl, &tf, &tb);
    float len = mx[0] - mn[0], wid = mx[1] - mn[1], hgt = mx[2] - mn[2];
    float r = fminf(fmaxf(hgt * 0.2f, 12.0f), len * 0.12f), w = fmaxf(wid * 0.12f, 8.0f);
    float cx = (mn[0] + mx[0]) * 0.5f;
    if (a->kind == VK_MOTORCYCLE) {
        float c0[3] = { mx[0] - r * 1.1f, 0, mn[2] + r }, c1[3] = { mn[0] + r * 1.1f, 0, mn[2] + r };
        set_wheel(&a->parts[a->nparts++], &tf, "Wheel_F", c0, r, w);
        set_wheel(&a->parts[a->nparts++], &tb, "Wheel_R", c1, r, w);
        return;
    }
    int per_side = a->kind == VK_TANK ? 6 : a->kind == VK_TRUCK ? 3 : 2;
    for (int i = 0; i < per_side; i++)
        for (int s = 0; s < 2; s++) {
            if (a->nparts >= VEH_MAX_PARTS) return;
            float fx = per_side == 1 ? 0.0f : (float)i / (float)(per_side - 1); // 0 front .. 1 back
            float x = (mx[0] - r * 1.4f) - fx * (len - r * 2.8f);
            float c[3] = { per_side > 2 || a->kind != VK_CAR ? x : (i == 0 ? cx + len * 0.31f : cx - len * 0.31f), s ? mx[1] - w * 0.5f : mn[1] + w * 0.5f, mn[2] + r };
            char nm[VEH_NAME];
            snprintf(nm, sizeof nm, "Wheel_%d%s", i + 1, s ? "R" : "L");
            set_wheel(&a->parts[a->nparts++], i == 0 ? &tf : &tb, nm, c, r, w);
            if (a->kind == VK_TANK) a->parts[a->nparts - 1].p[6] = 1.0f / (float)(per_side * 2);
        }
}

void veh_fit_to_body(VehicleAsset* a) {
    if (!a->body_mesh) return;
    float lmn[3], lmx[3];
    render_mesh_bounds(a->body_mesh, lmn, lmx);
    float mn[3], mx[3];
    for (int k = 0; k < 3; k++) mn[k] = lmn[k] * a->body_scale[k] + a->body_loc[k], mx[k] = lmx[k] * a->body_scale[k] + a->body_loc[k];
    if (fabsf(fmodf(a->body_rot[1], 180.0f)) > 45.0f) { // (turned a quarter: x and y swap)
        float c0 = (mn[0] + mx[0]) * 0.5f, c1 = (mn[1] + mx[1]) * 0.5f, h0 = (mx[0] - mn[0]) * 0.5f, h1 = (mx[1] - mn[1]) * 0.5f;
        mn[0] = c0 - h1, mx[0] = c0 + h1, mn[1] = c1 - h0, mx[1] = c1 + h0;
    }
    // the wheels without a model of their own go back under the corners
    uint32_t k = 0;
    for (uint32_t i = 0; i < a->nparts; i++)
        if (!(a->parts[i].kind == VP_WHEEL && !a->parts[i].mesh)) a->parts[k++] = a->parts[i];
    bool wheels = false;
    for (uint32_t i = 0; i < k; i++) wheels |= a->parts[i].kind == VP_WHEEL;
    a->nparts = k;
    static const int tmpl_of[VK_KIND_COUNT] = { VT_CAR, VT_MOTORCYCLE, VT_TRUCK, VT_TANK, VT_BOAT, VT_LOCOMOTIVE, VT_AIRPLANE, VT_HELICOPTER };
    VehicleAsset* t = calloc(1, sizeof *t); // (a template to copy from: an editor action, not a frame)
    if (!t) return;
    veh_template(t, tmpl_of[a->kind % VK_KIND_COUNT]);
    bool wheeled = a->kind == VK_CAR || a->kind == VK_MOTORCYCLE || a->kind == VK_TRUCK || a->kind == VK_TANK;
    if (wheeled && !wheels) place_wheels(a, t, mn, mx);
    for (int c = 0; c < 3; c++) a->extent[c] = fmaxf((mx[c] - mn[c]) * 0.5f, 10.0f);
    a->com[0] = (mn[0] + mx[0]) * 0.5f, a->com[1] = (mn[1] + mx[1]) * 0.5f, a->com[2] = mn[2] + (mx[2] - mn[2]) * 0.35f;
    free(t);
}

Entity vehicle_from_actors(World* w, const Entity* sel, uint32_t n, int tmpl, int front, const char* folder_vpath, char* err, size_t err_cap) {
    err[0] = 0;
    BuildPiece* pc = calloc(n ? n : 1, sizeof *pc); // (an editor action)
    if (!pc) return ENTITY_NONE;
    uint32_t np = 0;
    int body = -1;
    float best = -1.0f;
    float wmn[3] = { 1e30f, 1e30f, 1e30f }, wmx[3] = { -1e30f, -1e30f, -1e30f };
    for (uint32_t i = 0; i < n; i++) {
        const CTransform* t = ecs_get(w, sel[i], COMP_TRANSFORM);
        const CStaticMesh* m = ecs_get(w, sel[i], COMP_STATIC_MESH);
        const CName* nm = ecs_get(w, sel[i], COMP_NAME);
        if (!t || !m || ecs_get(w, sel[i], COMP_VEHICLE)) continue;
        BuildPiece* b = &pc[np];
        b->e = sel[i], b->mesh = m->mesh, b->material = m->material;
        snprintf(b->name, sizeof b->name, "%s", nm ? nm->text : "Part");
        piece_world_bounds(t, m->mesh, b->mn, b->mx);
        memcpy(b->rot, t->rotation, 12), memcpy(b->scale, t->scale, 12), memcpy(b->loc, t->location, 12);
        float vol = (b->mx[0] - b->mn[0]) * (b->mx[1] - b->mn[1]) * (b->mx[2] - b->mn[2]);
        b->role = name_has(b->name, k_wheel_words) ? 1 : name_has(b->name, k_door_words) ? 2 : 3;
        if (b->role != 1 && vol > best) best = vol, body = (int)np;
        for (int k = 0; k < 3; k++) wmn[k] = fminf(wmn[k], b->mn[k]), wmx[k] = fmaxf(wmx[k], b->mx[k]);
        np++;
    }
    if (body < 0 && np) { // (every piece named a wheel: the biggest is the body anyway)
        for (uint32_t i = 0; i < np; i++) {
            float vol = (pc[i].mx[0] - pc[i].mn[0]) * (pc[i].mx[1] - pc[i].mn[1]) * (pc[i].mx[2] - pc[i].mn[2]);
            if (vol > best) best = vol, body = (int)i;
        }
    }
    if (!np || body < 0) {
        snprintf(err, err_cap, "select the Static Mesh actors of the model (its body, and its wheels if they are apart)");
        free(pc);
        return ENTITY_NONE;
    }
    pc[body].role = 0;
    // no piece named a wheel: round pieces (two sides alike, the third thinner) in the lower half are the wheels
    bool named = false;
    for (uint32_t i = 0; i < np; i++) named |= pc[i].role == 1;
    float midz = (wmn[2] + wmx[2]) * 0.5f;
    if (!named)
        for (uint32_t i = 0; i < np; i++) {
            if ((int)i == body) continue;
            float s[3] = { pc[i].mx[0] - pc[i].mn[0], pc[i].mx[1] - pc[i].mn[1], pc[i].mx[2] - pc[i].mn[2] };
            float cz = (pc[i].mn[2] + pc[i].mx[2]) * 0.5f;
            bool round_y = fabsf(s[0] - s[2]) < 0.2f * fmaxf(s[0], s[2]) && s[1] < 0.8f * s[2];
            bool round_x = fabsf(s[1] - s[2]) < 0.2f * fmaxf(s[1], s[2]) && s[0] < 0.8f * s[2];
            if ((round_y || round_x) && cz < midz) pc[i].role = 1;
        }
    // the vehicle's frame: its place the whole model's center (x, y) at the body's center height; its front
    float wx = wmx[0] - wmn[0], wy = wmx[1] - wmn[1];
    if (front < 0) front = wy > wx * 1.15f ? 2 : 0; // (the longest side forward; +X, as the Unreal's imports)
    float yaw = front == 0 ? 0.0f : front == 1 ? 180.0f : front == 2 ? 90.0f : -90.0f;
    float org[3] = { (wmn[0] + wmx[0]) * 0.5f, (wmn[1] + wmx[1]) * 0.5f, (pc[body].mn[2] + pc[body].mx[2]) * 0.5f };
    float cy = cosf(yaw * 0.017453293f), sy = sinf(yaw * 0.017453293f);
    for (uint32_t i = 0; i < np; i++) { // world -> the vehicle's frame (turned by -yaw about the origin)
        BuildPiece* b = &pc[i];
        float c[3] = { (b->mn[0] + b->mx[0]) * 0.5f - org[0], (b->mn[1] + b->mx[1]) * 0.5f - org[1], (b->mn[2] + b->mx[2]) * 0.5f - org[2] };
        float h[3] = { (b->mx[0] - b->mn[0]) * 0.5f, (b->mx[1] - b->mn[1]) * 0.5f, (b->mx[2] - b->mn[2]) * 0.5f };
        float lc[3] = { c[0] * cy + c[1] * sy, -c[0] * sy + c[1] * cy, c[2] };
        if (front >= 2) { float t = h[0]; h[0] = h[1], h[1] = t; }
        for (int k = 0; k < 3; k++) b->mn[k] = lc[k] - h[k], b->mx[k] = lc[k] + h[k];
        float l[3] = { b->loc[0] - org[0], b->loc[1] - org[1], b->loc[2] - org[2] };
        b->loc[0] = l[0] * cy + l[1] * sy, b->loc[1] = -l[0] * sy + l[1] * cy, b->loc[2] = l[2];
        b->rot[1] -= yaw;
    }
    // the asset: the template's physics, the body's model, the wheels measured, the doors, the rest riding along
    const CName* bn = ecs_get(w, pc[body].e, COMP_NAME);
    char name[VEH_NAME];
    snprintf(name, sizeof name, "V_%.24s", bn ? bn->text : "Vehicle");
    for (char* c = name; *c; c++)
        if (!isalnum((unsigned char)*c) && *c != '_') *c = '_';
    uint32_t h = veh_create(folder_vpath, name, tmpl);
    VehicleAsset* a = h ? veh_find(h) : NULL;
    if (!a) {
        snprintf(err, err_cap, "could not save %s/%s.rvehicle", folder_vpath, name);
        free(pc);
        return ENTITY_NONE;
    }
    VehicleAsset* t = calloc(1, sizeof *t);
    if (!t) {
        free(pc);
        return ENTITY_NONE;
    }
    veh_template(t, tmpl);
    VehPart tf, tb;
    memset(&tf, 0, sizeof tf), memset(&tb, 0, sizeof tb);
    template_wheels(t, &tf, &tb);
    // the template's own parts: the engine, gearbox, differentials, wings, rotors, floats... (its wheels, doors, hood,
    // shocks and steering wheel were the boxes' look: gone; their physics comes back from the model's pieces)
    a->nparts = 0;
    float sx = (pc[body].mx[0] - pc[body].mn[0]) * 0.5f / fmaxf(t->extent[0], 1.0f), sy2 = (pc[body].mx[1] - pc[body].mn[1]) * 0.5f / fmaxf(t->extent[1], 1.0f);
    float sz = (pc[body].mx[2] - pc[body].mn[2]) * 0.5f / fmaxf(t->extent[2], 1.0f);
    for (uint32_t i = 0; i < t->nparts && a->nparts < VEH_MAX_PARTS; i++) {
        const VehPart* p = &t->parts[i];
        if (p->kind == VP_WHEEL || p->kind == VP_HINGE || p->kind == VP_PISTON || (p->kind == VP_GEAR && !strcmp(p->link, "Steer"))) continue;
        VehPart* q = &a->parts[a->nparts++];
        *q = *p;
        q->pos[0] *= sx, q->pos[1] *= sy2, q->pos[2] *= sz; // (wings, rotors, floats: where they were on a body this size)
        q->attach[0] = 0;
    }
    a->body_mesh = pc[body].mesh, a->body_material = pc[body].material;
    memcpy(a->body_loc, pc[body].loc, 12), memcpy(a->body_rot, pc[body].rot, 12), memcpy(a->body_scale, pc[body].scale, 12);
    int nwheels = 0, ndoors = 0, nother = 0;
    for (uint32_t i = 0; i < np && a->nparts < VEH_MAX_PARTS; i++) {
        BuildPiece* b = &pc[i];
        if ((int)i == body) continue;
        VehPart* q = &a->parts[a->nparts];
        float c[3] = { (b->mn[0] + b->mx[0]) * 0.5f, (b->mn[1] + b->mx[1]) * 0.5f, (b->mn[2] + b->mx[2]) * 0.5f };
        char pn[VEH_NAME];
        if (b->role == 1) {
            float r = fmaxf(b->mx[0] - b->mn[0], b->mx[2] - b->mn[2]) * 0.5f, wd = b->mx[1] - b->mn[1];
            snprintf(pn, sizeof pn, "Wheel_%s%s", c[0] >= 0 ? "F" : "R", c[1] >= 0 ? "R" : "L");
            for (uint32_t k = 0; k < a->nparts; k++) // (a name taken: numbered)
                if (!strcmp(a->parts[k].name, pn)) snprintf(pn, sizeof pn, "Wheel_%d", nwheels + 1);
            set_wheel(q, c[0] >= 0 ? &tf : &tb, pn, c, r, wd);
            nwheels++;
        } else {
            *q = (VehPart){ 0 };
            q->mscale[0] = q->mscale[1] = q->mscale[2] = 1.0f;
            if (b->role == 2) { // a door: on its front edge, swinging out (F opens)
                snprintf(pn, sizeof pn, "Door_%d", ++ndoors);
                q->kind = VP_HINGE, q->input = VI_ACTION1, q->axis[2] = 1.0f;
                q->pos[0] = b->mx[0], q->pos[1] = c[1], q->pos[2] = c[2];
                for (int k = 0; k < VEH_PARAMS; k++) q->p[k] = g_parts[VP_HINGE].def[k];
                if (c[1] >= 0) q->p[0] = -70, q->p[1] = 0;
                else q->p[0] = 0, q->p[1] = 70;
            } else { // rides on the body: a slider that never moves (its model drawn where it was)
                snprintf(pn, sizeof pn, "Part_%d", ++nother);
                q->kind = VP_SLIDER, q->axis[2] = 1.0f;
                for (int k = 0; k < VEH_PARAMS; k++) q->p[k] = g_parts[VP_SLIDER].def[k];
                q->p[0] = q->p[1] = 0.0f;
                memcpy(q->pos, c, 12);
            }
            snprintf(q->name, VEH_NAME, "%s", pn), snprintf(q->attach, VEH_NAME, "%s", pn);
        }
        // its model where it was: the component's place is the part's (pos; a wheel's hangs below), its turn and scale
        q->mesh = b->mesh, q->material = b->material;
        memcpy(q->mrot, b->rot, 12), memcpy(q->mscale, b->scale, 12);
        memcpy(q->mloc, b->loc, 12); // (the model's pivot where it was; a hinge turns it about its front edge, pos)
        a->nparts++;
    }
    veh_fit_to_body(a); // (the extent, the center of mass; wheels under the corners when none came apart)
    veh_save(a);
    free(t);
    // the vehicle's actor where the model was (its pieces gone); its wheels on the ground
    for (uint32_t i = 0; i < np; i++)
        if (ecs_alive(w, pc[i].e)) ecs_destroy(w, pc[i].e);
    float at[3] = { org[0], org[1], org[2] };
    Entity v = vehicle_spawn(w, a->hash, at, yaw, a->name);
    free(pc);
    if (v == ENTITY_NONE) snprintf(err, err_cap, "the vehicle's actor could not be made");
    return v;
}
