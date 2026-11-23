// The vehicles while playing (vehicle.c includes this after vehicle_sim.inc): the frame, the inputs, the trains, the
// parts' components, the camera, the rails' meshes, the self-test

const VehStats* vehicle_stats(void) { return &VS.stats; }

void vehicle_rebase(World* w, const float d[3]) {
    (void)w;
    Vec dm = vv(d[0] * 0.01f, d[1] * 0.01f, d[2] * 0.01f);
    for (uint32_t i = 0; i < VS.n; i++) VS.s[i]->x = vadd(VS.s[i]->x, dm);
    for (uint32_t i = 0; i < g_nrails; i++)
        for (uint32_t k = 0; k < g_rails[i]->n; k++) g_rails[i]->p[k][0] += dm.x, g_rails[i]->p[k][1] += dm.y, g_rails[i]->p[k][2] += dm.z;
    for (uint32_t i = 0; i < g_nwater; i++)
        g_water[i].mn[0] += dm.x, g_water[i].mx[0] += dm.x, g_water[i].mn[1] += dm.y, g_water[i].mx[1] += dm.y, g_water[i].z += dm.z;
}

static void rails_gather(World* w) {
    for (uint32_t i = 0; i < g_nrails; i++) free(g_rails[i]);
    g_nrails = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_RAIL)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
        const CRail* r = ecs_arch_column(ar, COMP_RAIL);
        const CTransform* t = ecs_arch_column(ar, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t k = 0; k < ar->count && g_nrails < 16; k++) {
            if (spline_kind(&r[k]) == SPLINE_KIND_CABLE) continue; // (Q14: a train runs on a Rail or any Spline, not a Cable)
            RailPath* rp = calloc(1, sizeof *rp); // (Begin Play)
            if (!rp) continue;
            rp->e = ents[k];
            rail_build(rp, &r[k], &t[k]);
            g_rails[g_nrails++] = rp;
        }
    }
}

void vehicle_begin_play(World* w) {
    vehicle_end_play(w);
    VS.playing = true;
    g_time = 0.0f;
    gather_water(w);
    rails_gather(w);
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_VEHICLE)) || !ar->count) continue;
        CVehicle* c = ecs_arch_column(ar, COMP_VEHICLE);
        for (uint32_t r = 0; r < ar->count; r++) c[r].inst = 0;
    }
}

void vehicle_end_play(World* w) {
    (void)w;
    for (uint32_t i = 0; i < VS.n; i++) free(VS.s[i]);
    VS.n = 0, VS.playing = false;
    for (uint32_t i = 0; i < g_nrails; i++) free(g_rails[i]);
    g_nrails = 0, g_nwater = 0;
}

static VState* state_make(World* w, Entity e, const VehicleAsset* a) {
    VState* s = state_of(e);
    if (!s) {
        if (VS.n >= VS_MAX) return NULL;
        s = calloc(1, sizeof *s); // (a vehicle's state: once, when it first runs)
        if (!s) return NULL;
        s->e = e;
        VS.s[VS.n++] = s;
    }
    Entity driver = s->driver;
    state_start(w, s, a);
    s->driver = driver;
    return s;
}

// The player's controls (when it drives this one): the axes and the keys
static void player_controls(VState* s) {
    float move[2] = { 0, 0 }, look[2] = { 0, 0 };
    bp_get_input(move, look);
    VehControls* c = &s->player;
    memset(c, 0, sizeof *c);
    bool shift = bp_key_held(340), ctrl = bp_key_held(341), q = bp_key_held('Q'), e = bp_key_held('E');
    if (s->a->kind == VK_AIRPLANE || s->a->kind == VK_HELICOPTER) {
        c->pitch = -move[0], c->roll = move[1], c->yaw = (float)e - (float)q;
        float lever = (float)shift - (float)ctrl;
        if (s->a->kind == VK_AIRPLANE) s->throttle_lever = clampf(s->throttle_lever + lever * 0.016f * 0.6f, 0.0f, 1.0f);
        else s->collective_lever = lever;
        c->brake = bp_key_held(' ') ? 1.0f : 0.0f;
        if (s->a->kind == VK_HELICOPTER) c->pitch = -move[0];
    } else {
        c->throttle = move[0], c->steer = move[1], c->yaw = (float)e - (float)q; // (S: a negative throttle - brake, then reverse)
        c->handbrake = bp_key_held(' ') ? 1.0f : 0.0f;
        if (s->manual) { // (Shift / Ctrl: up and down a gear)
            uint8_t edge = (uint8_t)(shift | (ctrl << 1));
            if ((edge & 1) && !(s->keys_prev & 0x10)) s->gear = s->gear < (int)s->a->ngears ? s->gear + 1 : s->gear;
            if ((edge & 2) && !(s->keys_prev & 0x20)) s->gear = s->gear > -1 ? s->gear - 1 : s->gear;
            s->keys_prev = (uint8_t)((s->keys_prev & 0x0F) | (edge << 4));
        }
    }
    s->cam_yaw_off = clampf(s->cam_yaw_off + look[0] * 0.15f, -180.0f, 180.0f);
    // F, G, H, J: the parts on Action 1-4 open / close
    static const int keys[4] = { 'F', 'G', 'H', 'J' };
    for (int k = 0; k < 4; k++) {
        bool d = bp_key_held(keys[k]), was = (s->keys_prev >> k) & 1u;
        if (d && !was)
            for (uint32_t i = 0; i < s->a->nparts; i++)
                if (s->a->parts[i].input == VI_ACTION1 + k) s->part_goal[i] = s->part_goal[i] > 0.5f ? 0.0f : 1.0f;
        s->keys_prev = (uint8_t)((s->keys_prev & ~(1u << k)) | ((uint8_t)d << k));
    }
}

// A train: along its rail (a leader drives the coupled cars behind it; they follow at their gaps)
static float half_len(const VehicleAsset* a) {
    float h = a->extent[0] * 0.01f;
    for (uint32_t i = 0; i < a->nparts; i++)
        if (a->parts[i].kind == VP_BOGIE) h = fmaxf(h, fabsf(a->parts[i].pos[0]) * 0.01f + 1.5f);
    return h;
}
static void train_place(World* w, VState* s) {
    const VehicleAsset* a = s->a;
    float bf = 0.0f, br = 0.0f, bz = 0.0f;
    bool any = false;
    for (uint32_t i = 0; i < a->nparts; i++)
        if (a->parts[i].kind == VP_BOGIE) {
            float x = a->parts[i].pos[0] * 0.01f;
            if (!any || x > bf) bf = any ? fmaxf(bf, x) : x;
            if (!any || x < br) br = any ? fminf(br, x) : x;
            bz = a->parts[i].pos[2] * 0.01f, any = true;
        }
    if (!any) bf = 1.0f, br = -1.0f;
    Vec tf, tr;
    Vec pf = rail_eval(s->rail, s->rail_s + bf, &tf), pr = rail_eval(s->rail, s->rail_s + br, &tr);
    Vec dir = vnorm(vsub(pf, pr));
    Vec side = vnorm(vcross(vv(0, 0, 1), dir)), upv = vcross(dir, side);
    Mat R = { { { dir.x, side.x, upv.x }, { dir.y, side.y, upv.y }, { dir.z, side.z, upv.z } } };
    Vec mid = vadd(pr, vmul(vsub(pf, pr), -br / fmaxf(bf - br, 1e-3f)));
    Vec loc = vsub(mid, mv(&R, vv(0, 0, bz)));
    s->q = matq(&R);
    s->x = vadd(loc, mv(&R, vmul(vof(a->com), 0.01f)));
    s->v = vmul(dir, s->rail_v), s->w = vv(0, 0, 0);
    (void)w;
}
static void train_step(World* w, VState* s, float dt, const VehControls* in) {
    if (!s->rail) return;
    if (s->leader) {
        s->rail_s = s->leader->rail_s - half_len(s->leader->a) - half_len(s->a) - ((const CVehicle*)ecs_get(w, s->e, COMP_VEHICLE))->couple_gap;
        s->rail_v = s->leader->rail_v;
        train_place(w, s);
        return;
    }
    // the whole train's mass, drag and brakes (the cars coupled behind)
    float mass = s->a->mass, drag = s->a->drag;
    for (uint32_t i = 0; i < VS.n; i++)
        for (VState* f = VS.s[i]->leader; f; f = f->leader)
            if (f == s) mass += VS.s[i]->a->mass, drag += VS.s[i]->a->drag;
    const VehPart *eng = NULL, *box = NULL;
    float rr = 0.5f;
    for (uint32_t i = 0; i < s->a->nparts; i++) {
        if (s->a->parts[i].kind == VP_ENGINE) eng = &s->a->parts[i];
        if (s->a->parts[i].kind == VP_GEARBOX) box = &s->a->parts[i];
        if (s->a->parts[i].kind == VP_WHEEL) rr = s->a->parts[i].p[0] * 0.01f;
    }
    Vec tan;
    rail_eval(s->rail, s->rail_s, &tan);
    float thr = s->engine_on ? clampf(in->throttle, 0.0f, 1.0f) : 0.0f, brk = clampf(in->brake, 0.0f, 1.0f);
    float fmax = eng ? eng->p[0] * (box ? box->p[0] : 1.0f) / rr : 0.0f;
    float traction = fminf(fmax, s->a->mass * 9.81f * 0.3f) * thr; // (the driving wheels' grip on the rail)
    if (s->a->max_speed > 0.0f && fabsf(s->rail_v) * 3.6f > s->a->max_speed) traction = 0.0f;
    float f = traction - mass * 9.81f * tan.z - 0.5f * 1.225f * drag * s->rail_v * fabsf(s->rail_v);
    float stop = brk * mass * 9.81f * 0.15f + mass * 9.81f * 0.002f; // (the brakes and the rolling resistance)
    s->rail_v += f / mass * dt;
    float dv = stop / mass * dt;
    s->rail_v = fabsf(s->rail_v) <= dv ? 0.0f : s->rail_v - copysignf(dv, s->rail_v);
    s->rail_s += s->rail_v * dt;
    if (!s->rail->closed && (s->rail_s < 0.0f || s->rail_s > s->rail->len)) s->rail_s = clampf(s->rail_s, 0.0f, s->rail->len), s->rail_v = 0.0f;
    s->rpm = eng ? eng->p[3] + (eng->p[2] - eng->p[3]) * clampf(fabsf(s->rail_v) / 30.0f, 0.0f, 1.0f) : 0.0f;
    train_place(w, s);
}

// The actor and its parts' components where the state is
static void place(World* w, VState* s, float dt) {
    const VehicleAsset* a = s->a;
    Mat R = qmat(s->q);
    CTransform* t = ecs_get(w, s->e, COMP_TRANSFORM);
    if (!t) return;
    Vec loc = vsub(s->x, mv(&R, vmul(vof(a->com), 0.01f)));
    t->location[0] = loc.x * 100.0f, t->location[1] = loc.y * 100.0f, t->location[2] = loc.z * 100.0f;
    mat_rot(&R, t->rotation);
    ecs_touch(w, s->e);
    for (uint32_t i = 0; i < a->nparts; i++) {
        const VehPart* p = &a->parts[i];
        // the moving parts: toward their goal at their speed (a free hinge swings with the motion)
        if (p->kind == VP_HINGE || p->kind == VP_SLIDER || (p->kind == VP_PISTON && !p->link[0])) {
            float range = p->kind == VP_HINGE ? fabsf(p->p[1] - p->p[0]) : fabsf(p->p[2] - p->p[1]);
            float speed = p->kind == VP_HINGE ? p->p[2] : p->kind == VP_SLIDER ? p->p[2] : p->p[3];
            float stepv = range > 1e-3f ? speed / range * dt : 1.0f;
            float d = s->part_goal[i] - s->part_t[i];
            s->part_t[i] += clampf(d, -stepv, stepv);
        }
        if (p->kind == VP_GEAR && !p->link[0]) s->part_spin[i] += p->p[1] * (VS_PI / 180.0f) * dt;
        Entity ce = s->part_ent[i];
        CAttach* at = ce ? ecs_get(w, ce, COMP_ATTACH) : NULL;
        if (!at) continue;
        Vec base = vof(s->part_base[i]);
        Mat brot = rot_mat(s->part_base[i] + 3);
        Vec nl = base;
        Mat nr = brot;
        Vec pos = vof(p->pos), ax = vnorm(vof(p->axis));
        switch (p->kind) {
        case VP_WHEEL: {
            const WheelState* ws = &s->wh[i];
            float droop = (p->p[4] - ws->comp * 100.0f);
            if (!ws->down) droop = p->p[4];
            nl = vsub(pos, vv(0, 0, droop));
            Mat yaw = axis_angle(vv(0, 0, 1), ws->steer), spin = axis_angle(vv(0, 1, 0), ws->spin);
            Mat ys = mmul(&yaw, &spin);
            nr = mmul(&ys, &brot);
            break;
        }
        case VP_HINGE: {
            float ang = (p->p[0] + (p->p[1] - p->p[0]) * s->part_t[i]) * (VS_PI / 180.0f);
            Mat r = axis_angle(ax, ang);
            nl = vadd(pos, mv(&r, vsub(base, pos)));
            nr = mmul(&r, &brot);
            break;
        }
        case VP_SLIDER: nl = vadd(base, vmul(ax, p->p[0] + (p->p[1] - p->p[0]) * s->part_t[i])); break;
        case VP_PISTON: {
            int li = p->link[0] ? part_named(a, p->link) : -1;
            Vec to;
            if (li >= 0 && s->part_ent[li]) { // (to the linked part's component: a shock to its wheel's hub)
                const CAttach* lat = ecs_get(w, s->part_ent[li], COMP_ATTACH);
                to = vof(lat->location);
                if (a->parts[li].kind == VP_HINGE) to = vadd(vof(a->parts[li].pos), vmul(vsub(vof(lat->location), vof(a->parts[li].pos)), 0.5f));
            } else to = vadd(pos, vmul(ax, p->p[1] + (p->p[2] - p->p[1]) * s->part_t[i] + p->p[0]));
            Vec d = vsub(to, pos);
            float len = vlen(d);
            Vec z = len > 1e-3f ? vmul(d, 1.0f / len) : vv(0, 0, 1);
            Vec xa = fabsf(z.z) < 0.95f ? vnorm(vcross(vv(0, 0, 1), z)) : vv(1, 0, 0), ya = vcross(z, xa);
            nr = (Mat){ { { xa.x, ya.x, z.x }, { xa.y, ya.y, z.y }, { xa.z, ya.z, z.z } } };
            nl = vadd(pos, vmul(d, 0.5f));
            at->scale[2] = len / 100.0f;
            break;
        }
        case VP_GEAR: {
            float ang = s->part_spin[i];
            if (!strcmp(p->link, "Steer")) ang = s->in.steer * p->p[0] * (VS_PI / 180.0f); // (the steering wheel: Ratio degrees at full lock)
            else if (!strcmp(p->link, "Engine")) s->part_spin[i] += s->rpm / 60.0f * 2.0f * VS_PI * p->p[0] * dt, ang = s->part_spin[i];
            else if (p->link[0]) {
                int li = part_named(a, p->link);
                if (li >= 0) ang = (a->parts[li].kind == VP_WHEEL ? s->wh[li].spin : s->part_spin[li]) * p->p[0];
            }
            if (!strcmp(p->name, "Turret")) ang = s->part_spin[i] += s->in.yaw * dt;
            Mat r = axis_angle(ax, ang);
            nr = mmul(&r, &brot);
            break;
        }
        case VP_PROPELLER: case VP_ROTOR: {
            Mat r = axis_angle(p->kind == VP_ROTOR ? vv(0, 0, 1) : ax, s->part_spin[i]);
            nr = mmul(&r, &brot);
            break;
        }
        case VP_WING: {
            if (!p->input) break;
            float ctrl = p->input == VI_PITCH ? -s->in.pitch : p->input == VI_ROLL ? s->in.roll * (p->pos[1] >= 0 ? -1.0f : 1.0f) : -(p->input == VI_YAW ? s->in.yaw : s->in.steer);
            Vec hinge = fabsf(ax.z) > 0.5f ? vv(0, 1, 0) : vv(0, 0, 1);
            Mat r = axis_angle(hinge, ctrl * p->p[4] * 0.5f * (VS_PI / 180.0f) * (fabsf(ax.z) > 0.5f ? 1.0f : -1.0f));
            nr = mmul(&r, &brot);
            break;
        }
        case VP_BOGIE: {
            if (!s->rail) break;
            Vec tb;
            Vec pw = rail_eval(s->rail, s->rail_s + p->pos[0] * 0.01f, &tb);
            Vec lp = mtv(&R, vsub(pw, loc));
            nl = vv(lp.x * 100.0f, lp.y * 100.0f, lp.z * 100.0f + p->p[0] * 0.6f);
            Vec tl = mtv(&R, tb);
            Mat r = axis_angle(vv(0, 0, 1), atan2f(tl.y, tl.x));
            nr = mmul(&r, &brot);
            break;
        }
        default: break;
        }
        at->location[0] = nl.x, at->location[1] = nl.y, at->location[2] = nl.z;
        mat_rot(&nr, at->rotation);
    }
    bp_components_place_actor(w, s->e);
}

// --- (Q27.4) Vehicles against each other, and the damage --------------------------------------------------------------
// A dent where it was hit: the impulse past 2500 N s (a 1300 kg car at ~7 km/h) pushes the grid's cells near the local
// point inward (0.004 cm per N s, a 60 cm falloff, 25 cm at most); a hit at the front crushes the engine's power (up to
// 80% lost), at a front wheel bends its toe (up to 0.06 rad: it pulls); the whole damage: the dents' mean over 10 cm
static void dent_at(VState* s, Vec local, float impulse) {
    const VehicleAsset* a = s->a;
    s->hits++;
    float over = impulse - 2500.0f;
    if (over <= 0.0f) return;
    float push = fminf(over * 0.004f, 25.0f), ex = a->extent[0], ey = a->extent[1], ez = a->extent[2];
    float lx = local.x * 100.0f, ly = local.y * 100.0f, lz = local.z * 100.0f; // (cm, from the box's center)
    float sum = 0.0f;
    for (int i = 0; i < 8; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) {
                float cx = ((float)i + 0.5f) / 8.0f * 2.0f * ex - ex, cy = ((float)j + 0.5f) / 4.0f * 2.0f * ey - ey, cz = ((float)k + 0.5f) / 4.0f * 2.0f * ez - ez;
                float d2 = (cx - lx) * (cx - lx) + (cy - ly) * (cy - ly) + (cz - lz) * (cz - lz), f = expf(-d2 / (2.0f * 60.0f * 60.0f));
                s->dent[i][j][k] = fminf(s->dent[i][j][k] + push * f, 25.0f);
                sum += s->dent[i][j][k];
            }
    s->damage = fminf(sum / (8.0f * 4.0f * 4.0f) / 10.0f, 1.0f);
    if (lx > ex * 0.5f) s->dmg_engine = fminf(s->dmg_engine + push / 25.0f * 0.35f, 0.8f); // (the front: the engine)
    if (lx > ex * 0.3f && fabsf(ly) > ey * 0.4f) { // (a front corner: that wheel's toe, toward the hit)
        int side = ly > 0.0f ? 1 : 0;
        s->dmg_toe[side] = clampf(s->dmg_toe[side] + (ly > 0.0f ? -1.0f : 1.0f) * push / 25.0f * 0.03f, -0.06f, 0.06f);
    }
}

// The 13 points of A's box (its corners, its bottom) inside B's box: an impulse between the two bodies at the deepest,
// pushed apart by their inverse masses; true when they touched
static bool collide_into(VState* A, VState* B) {
    const VehicleAsset *aa = A->a, *ab = B->a;
    Mat RA = qmat(A->q), RB = qmat(B->q);
    Vec comA = vmul(vof(aa->com), 0.01f), comB = vmul(vof(ab->com), 0.01f);
    float ax = aa->extent[0] * 0.01f, ay = aa->extent[1] * 0.01f, az = aa->extent[2] * 0.01f;
    float bx = ab->extent[0] * 0.01f, by = ab->extent[1] * 0.01f, bz = ab->extent[2] * 0.01f;
    Vec cB = vsub(B->x, mv(&RB, comB)); // (B's box center)
    static const float pts[13][3] = { { 1, 1, 1 }, { 1, -1, 1 }, { -1, 1, 1 }, { -1, -1, 1 }, { 1, 1, -1 }, { 1, -1, -1 }, { -1, 1, -1 }, { -1, -1, -1 },
                                      { 0, 0, -1 }, { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 } };
    float best = 0.0f;
    Vec bn = vv(0, 0, 0), bP = vv(0, 0, 0), bql = vv(0, 0, 0), bal = vv(0, 0, 0);
    for (int k = 0; k < 13; k++) {
        Vec la = vv(pts[k][0] * ax, pts[k][1] * ay, pts[k][2] * az);
        Vec P = vadd(A->x, mv(&RA, vsub(la, comA)));
        Vec q = mtv(&RB, vsub(P, cB));
        float px = bx - fabsf(q.x), py = by - fabsf(q.y), pz = bz - fabsf(q.z);
        if (px <= 0.0f || py <= 0.0f || pz <= 0.0f) continue;
        Vec nl = px <= py && px <= pz ? vv(q.x > 0 ? 1.0f : -1.0f, 0, 0) : py <= pz ? vv(0, q.y > 0 ? 1.0f : -1.0f, 0) : vv(0, 0, q.z > 0 ? 1.0f : -1.0f);
        float d = fminf(px, fminf(py, pz));
        if (d > best) best = d, bn = mv(&RB, nl), bP = P, bql = q, bal = la;
    }
    if (best <= 0.0f) return false;
    // (the normal from B to A; the velocities at the point)
    float mA = fmaxf(aa->mass, 1.0f), mB = fmaxf(ab->mass, 1.0f);
    Vec rA = vsub(bP, A->x), rB = vsub(bP, B->x);
    Vec vA = vadd(A->v, vcross(A->w, rA)), vB = vadd(B->v, vcross(B->w, rB));
    float vn = vdot(vsub(vA, vB), bn);
    // pushed apart (split by the masses)
    float sh = fminf(best, 0.1f), ka = mB / (mA + mB);
    A->x = vadd(A->x, vmul(bn, sh * ka)), B->x = vsub(B->x, vmul(bn, sh * (1.0f - ka)));
    if (vn >= 0.0f) return true;
    Vec lnA = mtv(&RA, vcross(rA, bn)), lnB = mtv(&RB, vcross(rB, bn));
    Vec ilA = mv(&RA, vv(lnA.x * A->inv_i[0], lnA.y * A->inv_i[1], lnA.z * A->inv_i[2]));
    Vec ilB = mv(&RB, vv(lnB.x * B->inv_i[0], lnB.y * B->inv_i[1], lnB.z * B->inv_i[2]));
    float kn = 1.0f / mA + 1.0f / mB + vdot(bn, vcross(ilA, rA)) + vdot(bn, vcross(ilB, rB));
    float j = -(1.0f + 0.1f) * vn / kn;
    Vec vt = vsub(vsub(vA, vB), vmul(bn, vn));
    float vtl = vlen(vt);
    Vec imp = vmul(bn, j);
    if (vtl > 1e-4f) imp = vsub(imp, vmul(vt, fminf(0.4f * j, vtl / kn) / vtl)); // (the panels scrape)
    A->v = vadd(A->v, vmul(imp, 1.0f / mA)), B->v = vsub(B->v, vmul(imp, 1.0f / mB));
    Vec liA = mtv(&RA, vcross(rA, imp)), liB = mtv(&RB, vcross(rB, imp));
    A->w = vadd(A->w, mv(&RA, vv(liA.x * A->inv_i[0], liA.y * A->inv_i[1], liA.z * A->inv_i[2])));
    B->w = vsub(B->w, mv(&RB, vv(liB.x * B->inv_i[0], liB.y * B->inv_i[1], liB.z * B->inv_i[2])));
    // the dents: A where its point went in, B where it was entered
    dent_at(A, bal, j);
    dent_at(B, bql, j);
    VS.stats.crashes++;
    return true;
}

static void vehicles_collide(World* w) {
    for (uint32_t i = 0; i < VS.n; i++) {
        VState* A = VS.s[i];
        if (!ecs_alive(w, A->e) || A->rail || A->a->kind == VK_TRAIN) continue;
        float ra = vlen(vof(A->a->extent)) * 0.01f;
        for (uint32_t j = i + 1; j < VS.n; j++) {
            VState* B = VS.s[j];
            if (!ecs_alive(w, B->e) || B->rail || B->a->kind == VK_TRAIN) continue;
            float rb = vlen(vof(B->a->extent)) * 0.01f;
            Vec d = vsub(A->x, B->x);
            if (vdot(d, d) > (ra + rb) * (ra + rb)) continue;
            if (!collide_into(A, B)) collide_into(B, A);
        }
    }
}

void vehicle_update(World* w, float dt) {
    if (!VS.playing) return;
    double t0 = re_time_seconds();
    if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;
    g_time += dt;
    VS.stats.vehicles = VS.stats.wheels = VS.stats.contacts = 0, VS.stats.crashes = 0;
    Entity player = bp_player(w);
    // the vehicles: states made (and remade when their asset was edited), coupled cars found
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_VEHICLE)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
        CVehicle* c = ecs_arch_column(ar, COMP_VEHICLE);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count; r++) {
            const VehicleAsset* a = veh_find(c[r].asset);
            if (!a) continue;
            VState* s = state_of(ents[r]);
            if (!s || s->a != a || s->version != a->version) {
                s = state_make(w, ents[r], a);
                if (s && c[r].auto_possess && player != ents[r]) vehicle_enter(w, ents[r]), player = bp_player(w);
            }
            c[r].inst = 1;
        }
    }
    for (uint32_t i = 0; i < VS.n; i++) { // (the leaders of the coupled cars)
        VState* s = VS.s[i];
        const CVehicle* c = ecs_get(w, s->e, COMP_VEHICLE);
        s->leader = NULL;
        if (!c || !c->couple_to[0]) continue;
        for (uint32_t j = 0; j < VS.n; j++) {
            const CName* n = ecs_get(w, VS.s[j]->e, COMP_NAME);
            if (j != i && n && !strcmp(n->text, c->couple_to)) s->leader = VS.s[j];
        }
    }
    float h = dt / VS_SUB;
    static float boxes[256][6];
    for (int pass = 0; pass < 2; pass++) // (the leaders first, then the cars that follow them)
        for (uint32_t i = 0; i < VS.n; i++) {
            VState* s = VS.s[i];
            if (!ecs_alive(w, s->e) || (pass == 0) != (s->leader == NULL)) continue;
            s->player_drives = player == s->e;
            if (s->player_drives) player_controls(s);
            VehControls in = s->player_drives ? s->player : s->in;
            if (!s->player_drives && (s->a->kind == VK_HELICOPTER)) s->collective_lever = in.collective;
            if (!s->player_drives && (s->a->kind == VK_AIRPLANE)) s->throttle_lever = clampf(in.throttle, 0.0f, 1.0f);
            s->in = in;
            s->contacts = 0;
            if (s->rail || s->a->kind == VK_TRAIN) {
                if (!s->rail) {
                    const CVehicle* c = ecs_get(w, s->e, COMP_VEHICLE);
                    if (c && c->rail[0]) s->rail = rail_named(w, c->rail);
                }
                if (s->rail) train_step(w, s, dt, &in);
            } else {
                // the level's boxes around it, once a frame
                Vec x = s->x;
                float ext = fmaxf(fmaxf(s->a->extent[0], s->a->extent[1]), s->a->extent[2]) + 300.0f + vlen(s->v) * dt * 100.0f;
                float mn[3] = { x.x * 100.0f - ext, x.y * 100.0f - ext, x.z * 100.0f - ext }, mx[3] = { x.x * 100.0f + ext, x.y * 100.0f + ext, x.z * 100.0f + ext };
                uint32_t nb = g_flat_ground ? 0u : bp_world_boxes(mn, mx, boxes, 256);
                for (int k = 0; k < VS_SUB; k++) step(w, s, h, &in, boxes, nb);
            }
            VS.stats.vehicles++, VS.stats.contacts += s->contacts;
            for (uint32_t k = 0; k < s->a->nparts; k++) VS.stats.wheels += s->a->parts[k].kind == VP_WHEEL;
        }
    vehicles_collide(w); // (Q27.4: against each other, then placed)
    for (uint32_t i = 0; i < VS.n; i++)
        if (ecs_alive(w, VS.s[i]->e)) place(w, VS.s[i], dt);
    VS.stats.ms = (re_time_seconds() - t0) * 1000.0;
}

// --- Inputs from Blueprints ---
static VState* st(World* w, Entity v) {
    VState* s = state_of(v);
    if (!s && VS.playing) {
        const CVehicle* c = ecs_get(w, v, COMP_VEHICLE);
        const VehicleAsset* a = c ? veh_find(c->asset) : NULL;
        if (a) s = state_make(w, v, a);
    }
    return s;
}
void vehicle_set_controls(World* w, Entity v, const VehControls* c) {
    VState* s = st(w, v);
    if (s) s->in = *c;
}
void vehicle_set_control(World* w, Entity v, int input, float value) {
    VState* s = st(w, v);
    if (!s) return;
    switch (input) {
    case VI_THROTTLE: s->in.throttle = value; break;
    case VI_STEER: s->in.steer = value; break;
    case VI_BRAKE: s->in.brake = value; break;
    case VI_HANDBRAKE: s->in.handbrake = value; break;
    case VI_PITCH: s->in.pitch = value; break;
    case VI_ROLL: s->in.roll = value; break;
    case VI_YAW: s->in.yaw = value; break;
    case VI_COLLECTIVE: s->in.collective = value; break;
    default: break;
    }
}
void vehicle_shift(World* w, Entity v, int gear) {
    VState* s = st(w, v);
    if (s) s->gear = gear < -1 ? -1 : gear > (int)s->a->ngears ? (int)s->a->ngears : gear, s->manual = true;
}
void vehicle_set_engine(World* w, Entity v, bool on) {
    VState* s = st(w, v);
    if (s) s->engine_on = on;
}
void vehicle_part_target(World* w, Entity v, const char* part, float t) {
    VState* s = st(w, v);
    int i = s && part ? part_named(s->a, part) : -1;
    if (i >= 0) s->part_goal[i] = clampf(t, 0.0f, 1.0f);
}
void vehicle_part_toggle(World* w, Entity v, const char* part) {
    VState* s = st(w, v);
    int i = s && part ? part_named(s->a, part) : -1;
    if (i >= 0) s->part_goal[i] = s->part_goal[i] > 0.5f ? 0.0f : 1.0f;
}
float vehicle_part_value(World* w, Entity v, const char* part) {
    VState* s = st(w, v);
    int i = s && part ? part_named(s->a, part) : -1;
    return i >= 0 ? s->part_t[i] : 0.0f;
}

bool vehicle_enter(World* w, Entity v) {
    VState* s = st(w, v);
    if (!s) return false;
    Entity pawn = bp_player(w);
    if (pawn == v) return true;
    if (pawn != ENTITY_NONE && !ecs_get(w, pawn, COMP_VEHICLE)) { // (the driver: hidden while it drives)
        s->driver = pawn;
        CTransform* pt = ecs_get(w, pawn, COMP_TRANSFORM);
        if (pt) pt->flags |= TRANSFORM_HIDDEN_GAME, ecs_touch(w, pawn);
    }
    s->engine_on = true;
    bp_set_player(w, v);
    return true;
}
bool vehicle_exit(World* w) {
    Entity v = bp_player(w);
    VState* s = v ? state_of(v) : NULL;
    if (!s || !s->driver || !ecs_alive(w, s->driver)) return false;
    Mat R = qmat(s->q);
    Vec side = mv(&R, vv(0, -1, 0)); // (out of the left door: -y)
    CTransform* pt = ecs_get(w, s->driver, COMP_TRANSFORM);
    if (pt) {
        Vec p = vadd(s->x, vmul(side, s->a->extent[1] * 0.01f + 1.0f));
        pt->location[0] = p.x * 100.0f, pt->location[1] = p.y * 100.0f, pt->location[2] = p.z * 100.0f + 60.0f;
        pt->flags &= ~TRANSFORM_HIDDEN_GAME;
        ecs_touch(w, s->driver);
    }
    bp_set_player(w, s->driver);
    s->driver = ENTITY_NONE;
    memset(&s->player, 0, sizeof s->player);
    return true;
}

bool vehicle_info(World* w, Entity v, VehInfo* o) {
    VState* s = state_of(v);
    if (!s) return false;
    memset(o, 0, sizeof *o);
    Mat R = qmat(s->q);
    Vec fwd = mv(&R, vv(1, 0, 0)), up = mv(&R, vv(0, 0, 1));
    o->speed_kmh = (s->rail ? s->rail_v : vdot(s->v, fwd)) * 3.6f;
    o->rpm = s->rpm, o->gear = s->gear, o->engine_on = s->engine_on, o->submerged = s->submerged, o->rail_s = s->rail_s;
    o->throttle = fminf(fmaxf(fmaxf(s->in.throttle, s->player.throttle), fmaxf(s->throttle_lever, 0.0f)), 1.0f);
    o->upright = up.z > 0.5f;
    for (uint32_t i = 0; i < s->a->nparts; i++)
        if (s->a->parts[i].kind == VP_WHEEL) o->wheels++, o->wheels_down += s->wh[i].down;
    const CTransform* t = ecs_get(w, v, COMP_TRANSFORM);
    if (t) memcpy(o->pos, t->location, 12);
    float a[3] = { s->x.x * 100.0f, s->x.y * 100.0f, s->x.z * 100.0f }, b[3] = { a[0], a[1], a[2] - 100000.0f };
    float f = g_trace(a, b, 0.0f);
    o->altitude_m = f < 1.0f ? f * 1000.0f : s->x.z;
    o->pitch_deg = asinf(clampf(fwd.z, -1.0f, 1.0f)) * (180.0f / VS_PI);
    o->roll_deg = asinf(clampf(-mv(&R, vv(0, 1, 0)).z, -1.0f, 1.0f)) * (180.0f / VS_PI); // (right wing (+y) down: positive)
    o->yaw_deg = atan2f(fwd.y, fwd.x) * (180.0f / VS_PI);
    o->damage = s->damage, o->engine_damage = s->dmg_engine, o->hits = s->hits; // (Q27.4)
    o->toe_deg[0] = s->dmg_toe[0] * (180.0f / VS_PI), o->toe_deg[1] = s->dmg_toe[1] * (180.0f / VS_PI);
    return true;
}

bool vehicle_camera(World* w, Entity v, float pos[3], float* yaw, float* pitch) {
    VState* s = state_of(v);
    if (!s) return false;
    Mat R = qmat(s->q);
    Vec fwd = mv(&R, vv(1, 0, 0));
    float heading = atan2f(fwd.y, fwd.x) * (180.0f / VS_PI);
    float d = heading - s->cam_yaw;
    while (d > 180.0f) d -= 360.0f;
    while (d < -180.0f) d += 360.0f;
    s->cam_yaw += d * 0.08f;
    s->cam_yaw_off *= 0.985f;
    float y = s->cam_yaw + s->cam_yaw_off, yr = y * (VS_PI / 180.0f);
    float dist = s->a->cam_distance, hgt = s->a->cam_height;
    pos[0] = s->x.x * 100.0f - cosf(yr) * dist, pos[1] = s->x.y * 100.0f - sinf(yr) * dist, pos[2] = s->x.z * 100.0f + hgt;
    *yaw = y, *pitch = -atan2f(hgt * 0.6f, dist) * (180.0f / VS_PI);
    (void)w;
    return true;
}

// --- The rails' meshes (two rails and the sleepers along the curve: the rail actor's components) ---
void vehicle_editor_update(World* w) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_RAIL)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
        for (uint32_t r = 0; r < ar->count; r++) {
            Entity e = ecs_arch_entities(ar)[r];
            CRail* rl = ecs_get(w, e, COMP_RAIL);
            const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
            if (spline_kind(rl) != SPLINE_KIND_RAIL) continue; // (Q14: a Spline's or a Cable's pieces are spline_world.c's)
            uint32_t hsh = 2166136261u;
            const uint8_t* bytes = (const uint8_t*)rl->points;
            for (size_t k = 0; k < sizeof(float) * 3 * rl->npoints; k++) hsh = (hsh ^ bytes[k]) * 16777619u;
            hsh ^= rl->npoints * 31u + rl->closed * 7u + (uint32_t)rl->gauge;
            for (uint32_t k = 0; k < rl->npoints && k < RAIL_MAX_POINTS; k++) // (Q14: a point's type and tangents; the old rails keep their hash)
                if (rl->ptype[k]) {
                    const uint8_t* xb = (const uint8_t*)rl->ptype;
                    for (size_t q = 0; q < sizeof rl->ptype + sizeof rl->arrive + sizeof rl->leave; q++) hsh = (hsh ^ (q < sizeof rl->ptype ? xb[q] : q < sizeof rl->ptype + sizeof rl->arrive ? ((const uint8_t*)rl->arrive)[q - sizeof rl->ptype] : ((const uint8_t*)rl->leave)[q - sizeof rl->ptype - sizeof rl->arrive])) * 16777619u;
                    break;
                }
            const uint8_t* tb = (const uint8_t*)t;
            for (size_t k = 0; k < sizeof(float) * 9; k++) hsh = (hsh ^ tb[k]) * 16777619u;
            if (!hsh) hsh = 1;
            if (rl->built == hsh) continue;
            // the old pieces go
            Entity old[1024];
            uint32_t nold = 0;
            for (uint32_t bi = 0; bi < w->archetype_count; bi++) {
                const EcsArchetype* br = &w->archetypes[bi];
                if (!(br->mask & COMP_BIT(COMP_ATTACH)) || !br->count) continue;
                const CAttach* at = ecs_arch_column(br, COMP_ATTACH);
                for (uint32_t k = 0; k < br->count && nold < 1024; k++)
                    if (at[k].owner == e) old[nold++] = ecs_arch_entities(br)[k];
            }
            for (uint32_t k = 0; k < nold; k++) ecs_destroy(w, old[k]);
            static RailPath rp; // (the editor's: one at a time)
            rail_build(&rp, ecs_get(w, e, COMP_RAIL), ecs_get(w, e, COMP_TRANSFORM));
            rl = ecs_get(w, e, COMP_RAIL), t = ecs_get(w, e, COMP_TRANSFORM);
            Mat m = rot_mat(t->rotation);
            float gauge = rl->gauge * 0.01f;
            uint32_t node = 1;
            const float steel[3] = { 0.35f, 0.33f, 0.32f }, wood[3] = { 0.18f, 0.11f, 0.06f };
            float seg = 6.0f;
            uint32_t nseg = (uint32_t)ceilf(rp.len / seg);
            if (nseg > 300) nseg = 300, seg = rp.len / 300.0f;
            for (uint32_t k = 0; k < nseg; k++) {
                float s0 = (float)k * seg, s1 = fminf(s0 + seg, rp.len);
                Vec ta, tb2;
                Vec p0 = rail_eval(&rp, s0, &ta), p1 = rail_eval(&rp, s1, &tb2);
                Vec d = vsub(p1, p0);
                float len = vlen(d);
                if (len < 1e-3f) continue;
                Vec dir = vmul(d, 1.0f / len), side = vnorm(vcross(vv(0, 0, 1), dir));
                float yawd = atan2f(dir.y, dir.x) * (180.0f / VS_PI), pitchd = asinf(clampf(dir.z, -1, 1)) * (180.0f / VS_PI);
                for (int sd = -1; sd <= 1; sd += 2) { // the two rails
                    Vec c = vadd(vmul(vadd(p0, p1), 0.5f), vmul(side, (float)sd * gauge * 0.5f));
                    Vec l = mtv(&m, vsub(vmul(c, 100.0f), vof(t->location)));
                    float loc[3] = { l.x, l.y, l.z + 8.0f }, rot[3] = { pitchd, yawd - t->rotation[1], 0 }, sc[3] = { len + 0.05f, 0.07f, 0.12f };
                    sc[0] = (len + 0.05f), sc[0] *= 1.0f;
                    float scl[3] = { sc[0], sc[1], sc[2] };
                    component(w, e, node++, "Rail", SHAPE_CUBE, loc, rot, scl, steel, 0.9f, 0.35f);
                }
                for (float ss = s0; ss < s1; ss += 0.65f * 1.0f + 0.0f) { // the sleepers
                    Vec tt;
                    Vec c = rail_eval(&rp, ss, &tt);
                    Vec l = mtv(&m, vsub(vmul(c, 100.0f), vof(t->location)));
                    float loc[3] = { l.x, l.y, l.z }, rot[3] = { 0, atan2f(tt.y, tt.x) * (180.0f / VS_PI) - t->rotation[1], 0 }, scl[3] = { 0.24f, gauge * 1.8f, 0.14f };
                    component(w, e, node++, "Sleeper", SHAPE_CUBE, loc, rot, scl, wood, 0.0f, 0.9f);
                    if (node > 4000) break;
                }
            }
            rl->built = hsh;
            bp_components_place_actor(w, e);
        }
    }
}

// --- The self-test: the vehicles on a flat floor at 0, without the level ---
static void drive(World* w, float seconds) {
    for (int f = 0; f < (int)(seconds * 60.0f); f++) vehicle_update(w, 1.0f / 60.0f);
}
int vehicle_selftest(void) {
    int fails = 0;
    // the tire's curve: grows, peaks, falls a little
    float a1 = magic(0.05f, 10, 1.9f), a2 = magic(0.12f, 10, 1.9f), a3 = magic(0.6f, 10, 1.9f);
    printf("[RealEngine] vehicle: the tire's longitudinal curve at 5%%, 12%%, 60%% slip: %.2f %.2f %.2f\n", a1, a2, a3);
    if (!(a1 < a2 && a3 < a2 && a2 > 0.9f)) printf("[RealEngine] FAIL: vehicle: the tire's curve\n"), fails++;
    static World w;
    ecs_init(&w);
    g_trace = flat_trace, g_flat_ground = true;
    VS.playing = true; // (no Begin Play: no level)
    vehicle_begin_play(&w);
    // a car: rests on its springs, pulls away, shifts up, reaches speed, brakes to a stop
    VehicleAsset* car = veh_temp("SelfTestCar", VT_CAR);
    Entity ce = vehicle_spawn(&w, car->hash, (float[3]){ 0, 0, 70 }, 0, "Car");
    drive(&w, 2.0f);
    VehInfo ci;
    vehicle_info(&w, ce, &ci);
    printf("[RealEngine] vehicle: the car at rest: %u/%u wheels down, its center %.1f cm up, %.2f km/h\n", ci.wheels_down, ci.wheels, ci.pos[2], ci.speed_kmh);
    if (ci.wheels_down != 4 || fabsf(ci.speed_kmh) > 0.5f || !ci.upright) printf("[RealEngine] FAIL: vehicle: the car at rest\n"), fails++;
    vehicle_set_control(&w, ce, VI_THROTTLE, 1.0f);
    drive(&w, 10.0f);
    vehicle_info(&w, ce, &ci);
    printf("[RealEngine] vehicle: full throttle 10 s: %.0f km/h in gear %d at %.0f rpm, upright %d\n", ci.speed_kmh, ci.gear, ci.rpm, ci.upright);
    if (ci.speed_kmh < 90.0f || ci.gear < 3 || !ci.upright) printf("[RealEngine] FAIL: vehicle: the car's acceleration and gears\n"), fails++;
    vehicle_set_control(&w, ce, VI_THROTTLE, 0.0f), vehicle_set_control(&w, ce, VI_STEER, 0.5f);
    drive(&w, 2.0f);
    vehicle_info(&w, ce, &ci);
    printf("[RealEngine] vehicle: steering at speed: heading %.0f deg, upright %d\n", ci.yaw_deg, ci.upright);
    if (fabsf(ci.yaw_deg) < 10.0f || !ci.upright) printf("[RealEngine] FAIL: vehicle: the car turning\n"), fails++;
    vehicle_set_control(&w, ce, VI_STEER, 0.0f), vehicle_set_control(&w, ce, VI_BRAKE, 1.0f);
    drive(&w, 6.0f);
    vehicle_info(&w, ce, &ci);
    printf("[RealEngine] vehicle: braking 6 s: %.2f km/h\n", ci.speed_kmh);
    if (fabsf(ci.speed_kmh) > 1.0f) printf("[RealEngine] FAIL: vehicle: the brakes\n"), fails++;
    vehicle_part_target(&w, ce, "Door_L", 1.0f);
    drive(&w, 1.0f);
    float door = vehicle_part_value(&w, ce, "Door_L");
    printf("[RealEngine] vehicle: the left door opened to %.2f\n", door);
    if (door < 0.99f) printf("[RealEngine] FAIL: vehicle: the door\n"), fails++;
    // a tank turns on the spot, then drives; a motorcycle rides upright through a curve
    VehicleAsset* tk = veh_temp("SelfTestTank", VT_TANK);
    Entity te = vehicle_spawn(&w, tk->hash, (float[3]){ 5000, 3000, veh_rest_height(tk) }, 0, "Tank");
    vehicle_set_control(&w, te, VI_STEER, 1.0f);
    drive(&w, 3.0f);
    VehInfo ti;
    vehicle_info(&w, te, &ti);
    float tyaw = ti.yaw_deg;
    vehicle_set_control(&w, te, VI_STEER, 0.0f), vehicle_set_control(&w, te, VI_THROTTLE, 1.0f);
    drive(&w, 5.0f);
    vehicle_info(&w, te, &ti);
    printf("[RealEngine] vehicle: the tank turned %.0f deg on the spot, then %.0f km/h, upright %d\n", tyaw, ti.speed_kmh, ti.upright);
    if (tyaw < 20.0f || tyaw > 200.0f || ti.speed_kmh < 10.0f || !ti.upright) printf("[RealEngine] FAIL: vehicle: the tank\n"), fails++;
    VehicleAsset* mc = veh_temp("SelfTestMoto", VT_MOTORCYCLE);
    Entity me = vehicle_spawn(&w, mc->hash, (float[3]){ -5000, -3000, veh_rest_height(mc) }, 0, "Moto");
    vehicle_set_control(&w, me, VI_THROTTLE, 0.8f);
    drive(&w, 4.0f);
    vehicle_set_control(&w, me, VI_STEER, -0.2f);
    drive(&w, 3.0f);
    VehInfo mi;
    vehicle_info(&w, me, &mi);
    printf("[RealEngine] vehicle: the motorcycle in a curve: %.0f km/h, leaning %.0f deg, upright %d\n", mi.speed_kmh, mi.roll_deg, mi.upright);
    if (mi.speed_kmh < 30.0f || !mi.upright) printf("[RealEngine] FAIL: vehicle: the motorcycle\n"), fails++;
    // a boat floats on a Water Body (no floor under it: g_flat_ground off, the water 2 m deep at 0)
    ecs_shutdown(&w);
    ecs_init(&w);
    vehicle_begin_play(&w);
    g_flat_ground = false, g_trace = flat_trace;
    water_spawn(&w, (float[3]){ 0, 0, 0 }, 100000, 100000);
    gather_water(&w);
    VehicleAsset* boat = veh_temp("SelfTestBoat", VT_BOAT);
    Entity be = vehicle_spawn(&w, boat->hash, (float[3]){ 0, 0, 100 }, 0, "Boat");
    g_flat_ground = false;
    drive(&w, 6.0f);
    VehInfo bi;
    vehicle_info(&w, be, &bi);
    printf("[RealEngine] vehicle: the boat after 6 s: %.0f cm (the water at 0), %.0f%% of its floats in, upright %d\n", bi.pos[2], bi.submerged * 100.0f, bi.upright);
    if (bi.pos[2] < -60.0f || bi.pos[2] > 80.0f || !bi.upright) printf("[RealEngine] FAIL: vehicle: the boat floating\n"), fails++;
    vehicle_set_control(&w, be, VI_THROTTLE, 1.0f);
    drive(&w, 8.0f);
    vehicle_info(&w, be, &bi);
    printf("[RealEngine] vehicle: the boat at full throttle: %.0f km/h, upright %d\n", bi.speed_kmh, bi.upright);
    if (bi.speed_kmh < 20.0f || !bi.upright) printf("[RealEngine] FAIL: vehicle: the boat's propeller\n"), fails++;
    // an airplane takes off (throttle full, then rotates and climbs)
    ecs_shutdown(&w);
    ecs_init(&w);
    vehicle_begin_play(&w);
    g_flat_ground = true;
    VehicleAsset* pl = veh_temp("SelfTestPlane", VT_AIRPLANE);
    Entity pe = vehicle_spawn(&w, pl->hash, (float[3]){ 0, 0, 110 }, 0, "Plane");
    VehInfo pi;
    float maxalt = 0.0f;
    for (int f = 0; f < 60 * 40; f++) {
        vehicle_info(&w, pe, &pi);
        VehControls c = { 0 };
        c.throttle = 1.0f;
        if (pi.speed_kmh > 110.0f) c.pitch = clampf((10.0f - pi.pitch_deg) * 0.08f, -1.0f, 1.0f); // (rotate, then hold 10 degrees nose up)
        c.roll = clampf(-pi.roll_deg * 0.05f, -1.0f, 1.0f);
        vehicle_set_controls(&w, pe, &c);
        vehicle_update(&w, 1.0f / 60.0f);
        maxalt = fmaxf(maxalt, pi.pos[2] / 100.0f);
    }
    vehicle_info(&w, pe, &pi);
    printf("[RealEngine] vehicle: the airplane after 40 s: %.0f m up, %.0f km/h, pitch %.0f, roll %.0f\n", pi.pos[2] / 100.0f, pi.speed_kmh, pi.pitch_deg, pi.roll_deg);
    if (pi.pos[2] / 100.0f < 30.0f || fabsf(pi.roll_deg) > 30.0f) printf("[RealEngine] FAIL: vehicle: the airplane's takeoff\n"), fails++;
    // a helicopter lifts off and hovers when let go (Stability)
    ecs_shutdown(&w);
    ecs_init(&w);
    vehicle_begin_play(&w);
    VehicleAsset* hc = veh_temp("SelfTestHeli", VT_HELICOPTER);
    Entity he = vehicle_spawn(&w, hc->hash, (float[3]){ 0, 0, 110 }, 0, "Heli");
    VehControls hcol = { 0 };
    hcol.collective = 0.6f;
    vehicle_set_controls(&w, he, &hcol);
    drive(&w, 4.0f);
    hcol.collective = 0.0f;
    vehicle_set_controls(&w, he, &hcol);
    VehInfo hi;
    drive(&w, 2.0f);
    vehicle_info(&w, he, &hi);
    float z1 = hi.pos[2];
    drive(&w, 3.0f);
    vehicle_info(&w, he, &hi);
    printf("[RealEngine] vehicle: the helicopter let go: %.0f cm, 3 s later %.0f cm, upright %d\n", z1, hi.pos[2], hi.upright);
    if (z1 < 500.0f || fabsf(hi.pos[2] - z1) > 300.0f || !hi.upright) printf("[RealEngine] FAIL: vehicle: the helicopter's hover\n"), fails++;
    // a train with a wagon on a loop
    ecs_shutdown(&w);
    ecs_init(&w);
    const float loop[4][3] = { { 0, 0, 0 }, { 30000, 0, 0 }, { 30000, 20000, 0 }, { 0, 20000, 0 } };
    rail_spawn(&w, loop, 4, true, "TestRail");
    vehicle_begin_play(&w);
    VehicleAsset* lo = veh_temp("SelfTestLoco", VT_LOCOMOTIVE);
    VehicleAsset* wa = veh_temp("SelfTestWagon", VT_WAGON);
    Entity le = vehicle_spawn(&w, lo->hash, (float[3]){ 0, 0, 0 }, 0, "Loco");
    Entity we = vehicle_spawn(&w, wa->hash, (float[3]){ 0, 0, 0 }, 0, "Wagon");
    CVehicle* lc = ecs_get(&w, le, COMP_VEHICLE);
    snprintf(lc->rail, sizeof lc->rail, "TestRail"), lc->rail_start = 100.0f;
    CVehicle* wc = ecs_get(&w, we, COMP_VEHICLE);
    snprintf(wc->rail, sizeof wc->rail, "TestRail"), snprintf(wc->couple_to, sizeof wc->couple_to, "Loco");
    vehicle_set_control(&w, le, VI_THROTTLE, 1.0f);
    drive(&w, 20.0f);
    VehInfo li, wi;
    vehicle_info(&w, le, &li), vehicle_info(&w, we, &wi);
    printf("[RealEngine] vehicle: the train after 20 s: %.0f km/h, %.0f m along, its wagon %.1f m behind\n", li.speed_kmh, li.rail_s - 100.0f, li.rail_s - wi.rail_s);
    if (li.speed_kmh < 20.0f || li.rail_s - wi.rail_s < 15.0f || li.rail_s - wi.rail_s > 25.0f) printf("[RealEngine] FAIL: vehicle: the train\n"), fails++;
    vehicle_end_play(&w);
    ecs_shutdown(&w);
    g_trace = bp_trace_boxes, g_flat_ground = false;
    printf("[RealEngine] vehicle self-test: %s\n", fails ? "FAIL" : "ok");
    return fails;
}

// --vehicle-mesh-test (the editor's renderer up: the meshes' bounds): an "imported" car (a body and four wheels apart,
// named as a model's are, the wheels lying on their side) made into a vehicle by vehicle_from_actors; it rests on its
// four wheels, pulls away and steers. Its asset is written to /Content/__VehicleTest and removed afterwards
int vehicle_mesh_selftest(const char* content_root) {
    int fails = 0;
    static World w;
    ecs_init(&w);
    vehicle_set_root(content_root);
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/__VehicleTest", content_root);
    re_make_dirs(dir);
    Entity sel[5];
    uint32_t n = 0;
    const char* names[5] = { "Car_Body", "Wheel_FL", "Wheel_FR", "Wheel_RL", "Wheel_RR" };
    for (int i = 0; i < 5; i++) { // (the shapes are 100 cm: the body 4.2 x 1.8 x 0.9 m, the wheels 66 cm across, 22 wide)
        Entity e = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
        snprintf(((CName*)ecs_get(&w, e, COMP_NAME))->text, RE_NAME_MAX, "%s", names[i]);
        CTransform* t = ecs_get(&w, e, COMP_TRANSFORM);
        CStaticMesh* m = ecs_get(&w, e, COMP_STATIC_MESH);
        t->flags = MOBILITY_MOVABLE;
        if (!i) {
            m->mesh = SHAPE_CUBE;
            t->location[0] = 1000, t->location[1] = 500, t->location[2] = 75;
            t->scale[0] = 4.2f, t->scale[1] = 1.8f, t->scale[2] = 0.9f;
        } else {
            m->mesh = SHAPE_CYLINDER;
            t->location[0] = 1000 + (i <= 2 ? 135.0f : -130.0f), t->location[1] = 500 + (i & 1 ? -80.0f : 80.0f), t->location[2] = 33;
            t->rotation[2] = 90.0f; // (the cylinder's axis turned across: an axle along Y)
            t->scale[0] = t->scale[1] = 0.66f, t->scale[2] = 0.22f;
        }
        sel[n++] = e;
    }
    char err[200];
    Entity v = vehicle_from_actors(&w, sel, n, VT_CAR, -1, "/Content/__VehicleTest", err, sizeof err);
    const VehicleAsset* a = v != ENTITY_NONE ? veh_find(((CVehicle*)ecs_get(&w, v, COMP_VEHICLE))->asset) : NULL;
    uint32_t wheels = 0, meshes = 0;
    float rad = 0.0f;
    for (uint32_t i = 0; a && i < a->nparts; i++)
        if (a->parts[i].kind == VP_WHEEL) wheels++, meshes += a->parts[i].mesh != 0, rad = a->parts[i].p[0];
    printf("[RealEngine] vehicle from meshes: %s, %u wheels (%u with their mesh, radius %.0f cm), body mesh %s %s\n", a ? a->name : err, wheels,
           meshes, rad, a && a->body_mesh ? "yes" : "no", err);
    if (!a || wheels != 4 || meshes != 4 || fabsf(rad - 33.0f) > 2.0f) printf("[RealEngine] FAIL: vehicle from meshes: the asset\n"), fails++;
    if (a) {
        g_trace = flat_trace, g_flat_ground = true;
        VS.playing = true;
        vehicle_begin_play(&w);
        drive(&w, 2.0f);
        VehInfo ci;
        vehicle_info(&w, v, &ci);
        printf("[RealEngine] vehicle from meshes at rest: %u/%u wheels down, %.2f km/h, upright %d\n", ci.wheels_down, ci.wheels, ci.speed_kmh, ci.upright);
        if (ci.wheels_down != 4 || !ci.upright) printf("[RealEngine] FAIL: vehicle from meshes: at rest\n"), fails++;
        vehicle_set_control(&w, v, VI_THROTTLE, 1.0f);
        drive(&w, 5.0f);
        vehicle_set_control(&w, v, VI_STEER, 0.5f);
        drive(&w, 2.0f);
        vehicle_info(&w, v, &ci);
        printf("[RealEngine] vehicle from meshes driven 7 s: %.0f km/h, heading %.0f deg, upright %d\n", ci.speed_kmh, ci.yaw_deg, ci.upright);
        if (ci.speed_kmh < 40.0f || fabsf(ci.yaw_deg) < 10.0f || !ci.upright) printf("[RealEngine] FAIL: vehicle from meshes: driving\n"), fails++;
        vehicle_end_play(&w);
        g_trace = bp_trace_boxes, g_flat_ground = false;
    }
    ecs_shutdown(&w);
    re_remove_tree(dir);
    VA.scanned = false;
    printf("[RealEngine] vehicle from meshes self-test: %s\n", fails ? "FAIL" : "ok");
    return fails;
}
