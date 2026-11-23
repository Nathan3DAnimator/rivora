// The vehicles' simulation (vehicle.c includes this): SI units inside (m, kg, s, N), the world's cm at the edges.
// 4 fixed substeps a frame; nothing allocated while playing (the states are made at Begin Play / the first frame).

#define VS_MAX 256
#define VS_SUB 4
#define VS_PI 3.14159265f

typedef struct Vec { float x, y, z; } Vec;
static Vec vv(float x, float y, float z) { return (Vec){ x, y, z }; }
static Vec vadd(Vec a, Vec b) { return vv(a.x + b.x, a.y + b.y, a.z + b.z); }
static Vec vsub(Vec a, Vec b) { return vv(a.x - b.x, a.y - b.y, a.z - b.z); }
static Vec vmul(Vec a, float s) { return vv(a.x * s, a.y * s, a.z * s); }
static float vdot(Vec a, Vec b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static Vec vcross(Vec a, Vec b) { return vv(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x); }
static float vlen(Vec a) { return sqrtf(vdot(a, a)); }
static Vec vnorm(Vec a) {
    float l = vlen(a);
    return l > 1e-9f ? vmul(a, 1.0f / l) : vv(0, 0, 1);
}
static Vec vof(const float* p) { return vv(p[0], p[1], p[2]); }
static float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }

typedef struct Mat { float m[3][3]; } Mat; // columns: the local axes in the world
static Vec mv(const Mat* r, Vec v) {
    return vv(r->m[0][0] * v.x + r->m[0][1] * v.y + r->m[0][2] * v.z, r->m[1][0] * v.x + r->m[1][1] * v.y + r->m[1][2] * v.z,
              r->m[2][0] * v.x + r->m[2][1] * v.y + r->m[2][2] * v.z);
}
static Vec mtv(const Mat* r, Vec v) {
    return vv(r->m[0][0] * v.x + r->m[1][0] * v.y + r->m[2][0] * v.z, r->m[0][1] * v.x + r->m[1][1] * v.y + r->m[2][1] * v.z,
              r->m[0][2] * v.x + r->m[1][2] * v.y + r->m[2][2] * v.z);
}
typedef struct Quat { float x, y, z, w; } Quat;
static Quat qn(Quat q) {
    float l = sqrtf(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
    return l > 1e-12f ? (Quat){ q.x / l, q.y / l, q.z / l, q.w / l } : (Quat){ 0, 0, 0, 1 };
}
static Mat qmat(Quat q) {
    float x = q.x, y = q.y, z = q.z, w = q.w;
    return (Mat){ { { 1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w) },
                    { 2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w) },
                    { 2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y) } } };
}
static Quat matq(const Mat* m) {
    float tr = m->m[0][0] + m->m[1][1] + m->m[2][2];
    Quat q;
    if (tr > 0.0f) {
        float s = sqrtf(tr + 1.0f) * 2.0f;
        q = (Quat){ (m->m[2][1] - m->m[1][2]) / s, (m->m[0][2] - m->m[2][0]) / s, (m->m[1][0] - m->m[0][1]) / s, 0.25f * s };
    } else if (m->m[0][0] > m->m[1][1] && m->m[0][0] > m->m[2][2]) {
        float s = sqrtf(1.0f + m->m[0][0] - m->m[1][1] - m->m[2][2]) * 2.0f;
        q = (Quat){ 0.25f * s, (m->m[0][1] + m->m[1][0]) / s, (m->m[0][2] + m->m[2][0]) / s, (m->m[2][1] - m->m[1][2]) / s };
    } else if (m->m[1][1] > m->m[2][2]) {
        float s = sqrtf(1.0f + m->m[1][1] - m->m[0][0] - m->m[2][2]) * 2.0f;
        q = (Quat){ (m->m[0][1] + m->m[1][0]) / s, 0.25f * s, (m->m[1][2] + m->m[2][1]) / s, (m->m[0][2] - m->m[2][0]) / s };
    } else {
        float s = sqrtf(1.0f + m->m[2][2] - m->m[0][0] - m->m[1][1]) * 2.0f;
        q = (Quat){ (m->m[0][2] + m->m[2][0]) / s, (m->m[1][2] + m->m[2][1]) / s, 0.25f * s, (m->m[1][0] - m->m[0][1]) / s };
    }
    return qn(q);
}
// The transform's rotator (degrees) <-> a matrix (rotator_axes: rows are the local axes)
static Mat rot_mat(const float rot[3]) {
    float ax[3][3];
    rotator_axes(rot, ax);
    Mat r;
    for (int a = 0; a < 3; a++)
        for (int k = 0; k < 3; k++) r.m[k][a] = ax[a][k];
    return r;
}
static void mat_rot(const Mat* r, float rot[3]) {
    float ax[3][3];
    for (int a = 0; a < 3; a++)
        for (int k = 0; k < 3; k++) ax[a][k] = r->m[k][a];
    axes_rotator(ax, rot);
}
static Mat axis_angle(Vec a, float ang) {
    a = vnorm(a);
    float c = cosf(ang), s = sinf(ang), k = 1.0f - c;
    return (Mat){ { { c + a.x * a.x * k, a.x * a.y * k - a.z * s, a.x * a.z * k + a.y * s },
                    { a.y * a.x * k + a.z * s, c + a.y * a.y * k, a.y * a.z * k - a.x * s },
                    { a.z * a.x * k - a.y * s, a.z * a.y * k + a.x * s, c + a.z * a.z * k } } };
}
static Mat mmul(const Mat* a, const Mat* b) {
    Mat r;
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a->m[i][0] * b->m[0][j] + a->m[i][1] * b->m[1][j] + a->m[i][2] * b->m[2][j];
    return r;
}

// --- The ground: the level's boxes and landscapes (the self-test's: a flat floor at 0) ---
static float (*g_trace)(const float a[3], const float b[3], float radius) = bp_trace_boxes;
static bool g_flat_ground; // (the self-test)
static float flat_trace(const float a[3], const float b[3], float radius) {
    (void)radius;
    if (a[2] <= 0.0f) return 0.0f;
    if (b[2] >= 0.0f) return 1.0f;
    return a[2] / (a[2] - b[2]);
}

// --- Water bodies (gathered once a frame) ---
typedef struct WaterBody { float mn[2], mx[2], z, wave_h, wave_k, wave_w, cur[2], rho, bottom; bool ocean; int river; } WaterBody;
static WaterBody g_water[32];
static uint32_t g_nwater;
// (B12c) the rivers' samples (world cm), a body's river its index here
static RiverSample g_river[4][RIVER_SAMPLES];
static uint32_t g_nriver_s[4], g_nriver;
static float g_time;
static void gather_water(World* w) {
    g_nwater = 0, g_nriver = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_WATER)) || !(ar->mask & COMP_BIT(COMP_TRANSFORM)) || !ar->count) continue;
        const CWater* cw = ecs_arch_column(ar, COMP_WATER);
        const CTransform* t = ecs_arch_column(ar, COMP_TRANSFORM);
        for (uint32_t r = 0; r < ar->count && g_nwater < 32; r++) {
            if (cw[r].exclusion) continue; // (a hole in the ocean, not water: water_ocean_height says none inside it)
            const CRiver* rv = (ar->mask & COMP_BIT(COMP_RIVER)) ? &((const CRiver*)ecs_arch_column(ar, COMP_RIVER))[r] : NULL;
            if (rv && g_nriver >= 4) continue;
            WaterBody* b = &g_water[g_nwater++];
            b->river = -1;
            if (rv) { // a river: its samples; the box around them (the quick test)
                uint32_t ri = g_nriver;
                g_nriver_s[ri] = river_samples(rv, &t[r], g_river[ri], RIVER_SAMPLES);
                if (!g_nriver_s[ri]) { g_nwater--; continue; }
                g_nriver++;
                b->river = (int)ri, b->ocean = false;
                b->mn[0] = b->mn[1] = 1e30f, b->mx[0] = b->mx[1] = -1e30f;
                for (uint32_t k = 0; k < g_nriver_s[ri]; k++) {
                    const RiverSample* s = &g_river[ri][k];
                    for (int c = 0; c < 2; c++) b->mn[c] = fminf(b->mn[c], (s->p[c] - s->width) / 100.0f), b->mx[c] = fmaxf(b->mx[c], (s->p[c] + s->width) / 100.0f);
                }
                b->z = t[r].location[2] / 100.0f, b->wave_h = 0.0f, b->wave_k = b->wave_w = 0.0f, b->cur[0] = b->cur[1] = 0.0f;
                b->rho = cw[r].density > 1.0f ? cw[r].density : 1000.0f, b->bottom = b->z - 10.0f;
                continue;
            }
            float hx = 50.0f * fabsf(t[r].scale[0]), hy = 50.0f * fabsf(t[r].scale[1]);
            b->mn[0] = (t[r].location[0] - hx) / 100.0f, b->mx[0] = (t[r].location[0] + hx) / 100.0f;
            b->mn[1] = (t[r].location[1] - hy) / 100.0f, b->mx[1] = (t[r].location[1] + hy) / 100.0f;
            b->z = t[r].location[2] / 100.0f, b->wave_h = cw[r].wave_height / 100.0f;
            b->wave_k = cw[r].wave_length > 1.0f ? 2.0f * VS_PI / (cw[r].wave_length / 100.0f) : 0.0f;
            b->wave_w = b->wave_k * cw[r].wave_speed / 100.0f;
            b->cur[0] = cw[r].current[0] / 100.0f, b->cur[1] = cw[r].current[1] / 100.0f;
            b->rho = cw[r].density > 1.0f ? cw[r].density : 1000.0f, b->bottom = b->z - cw[r].depth / 100.0f;
            b->ocean = cw[r].ocean != 0; // (endless; its waves the FFT's biggest: water.c water_ocean_height)
        }
    }
}
// (m) the surface at x, y: two crossing wave trains
static const WaterBody* water_at(float x, float y, float* z) {
    for (uint32_t i = 0; i < g_nwater; i++) {
        const WaterBody* b = &g_water[i];
        float oz;
        if (b->ocean && water_ocean_height(x * 100.0f, y * 100.0f, &oz)) { *z = oz * 0.01f; return b; }
        if (x < b->mn[0] || x > b->mx[0] || y < b->mn[1] || y > b->mx[1]) continue;
        if (b->river >= 0) { // (its own current, its own height along it: water_query)
            float rz, cur[2], dep;
            if (!river_at(g_river[b->river], g_nriver_s[b->river], x * 100.0f, y * 100.0f, &rz, cur, &dep)) continue;
            *z = rz * 0.01f;
            return b;
        }
        *z = b->z + b->wave_h * (0.6f * sinf(b->wave_k * x - b->wave_w * g_time) + 0.4f * sinf(b->wave_k * 0.7f * (x * 0.6f + y * 0.8f) - b->wave_w * 0.8f * g_time));
        return b;
    }
    return NULL;
}
// (B12f) The water at a point (cm): its surface's height there, its current (cm/s), its density (kg/m^3), how deep it
// is; false: no water there. The bodies gathered by water_gather (once a frame: physics_update, the characters' move)
void water_gather(World* w) { gather_water(w); }
bool water_query(World* w, const float p[3], float* level, float cur[2], float* rho, float* depth) {
    (void)w;
    float zz;
    const WaterBody* b = water_at(p[0] / 100.0f, p[1] / 100.0f, &zz);
    if (!b) return false;
    *level = zz * 100.0f, *rho = b->rho;
    cur[0] = b->cur[0] * 100.0f, cur[1] = b->cur[1] * 100.0f;
    *depth = (zz - b->bottom) * 100.0f;
    if (b->river >= 0) {
        float rz, dep;
        river_at(g_river[b->river], g_nriver_s[b->river], p[0], p[1], &rz, cur, &dep);
        *depth = dep;
    }
    return true;
}
bool water_height(World* w, float x, float y, float* z) {
    if (!g_nwater) gather_water(w);
    float zz;
    if (!water_at(x / 100.0f, y / 100.0f, &zz)) return false;
    *z = zz * 100.0f;
    return true;
}

// --- Rails (Catmull-Rom through the points; an arc-length table) ---
#define RAIL_SAMPLES 24
typedef struct RailPath {
    Entity e;
    uint32_t n;            // samples
    float len;             // m
    float p[RAIL_MAX_POINTS * RAIL_SAMPLES + 1][3]; // m, world
    float s[RAIL_MAX_POINTS * RAIL_SAMPLES + 1];    // m along
    bool closed;
} RailPath;
static RailPath* g_rails[16];
static uint32_t g_nrails;
// (Q14) the rail's curve is the engine's one (spline.h): a Rail's points are Curve points (Catmull-Rom, as before) unless
// its Details give them another type or tangents
static void rail_build(RailPath* rp, const CRail* r, const CTransform* t) {
    float rows[3][4];
    render_transform_rows(t, rows);
    static SplineCurve c; // (the main thread's: Begin Play, the editor's rebuild)
    spline_curve_from(&c, r, rows);
    uint32_t segs = spline_segment_count(&c);
    rp->n = 0, rp->closed = c.closed;
    for (uint32_t sgi = 0; sgi < segs; sgi++)
        for (int k = 0; k < RAIL_SAMPLES; k++) {
            float* o = rp->p[rp->n++];
            spline_eval_key(&c, (float)sgi + (float)k / RAIL_SAMPLES, o, NULL);
            o[0] *= 0.01f, o[1] *= 0.01f, o[2] *= 0.01f;
        }
    const float* last = c.n ? (c.closed ? c.p[0] : c.p[c.n - 1]) : rows[0]; // (no point: the actor's place below)
    float* o = rp->p[rp->n++];
    if (c.n) o[0] = last[0] * 0.01f, o[1] = last[1] * 0.01f, o[2] = last[2] * 0.01f;
    else o[0] = rows[0][3] * 0.01f, o[1] = rows[1][3] * 0.01f, o[2] = rows[2][3] * 0.01f;
    if (rp->n < 2) memcpy(rp->p[rp->n++], o, 12); // (a single point: a rail of no length)
    rp->s[0] = 0.0f;
    for (uint32_t i = 1; i < rp->n; i++) rp->s[i] = rp->s[i - 1] + vlen(vsub(vof(rp->p[i]), vof(rp->p[i - 1])));
    rp->len = rp->s[rp->n - 1];
}
// (m) the place and the direction at s along the rail (a loop wraps; an open rail stops at its ends)
static Vec rail_eval(const RailPath* rp, float s, Vec* tangent) {
    if (rp->closed && rp->len > 0.0f) s = fmodf(s, rp->len), s = s < 0.0f ? s + rp->len : s;
    else s = clampf(s, 0.0f, rp->len);
    uint32_t lo = 0, hi = rp->n - 1;
    while (hi - lo > 1) {
        uint32_t mid = (lo + hi) / 2;
        if (rp->s[mid] <= s) lo = mid;
        else hi = mid;
    }
    float seg = rp->s[hi] - rp->s[lo], t = seg > 1e-6f ? (s - rp->s[lo]) / seg : 0.0f;
    Vec a = vof(rp->p[lo]), b = vof(rp->p[hi]);
    if (tangent) *tangent = vnorm(vsub(b, a));
    return vadd(a, vmul(vsub(b, a), t));
}
static RailPath* rail_named(World* w, const char* name) {
    for (uint32_t i = 0; i < g_nrails; i++) {
        const CName* n = ecs_get(w, g_rails[i]->e, COMP_NAME);
        if (n && !strcmp(n->text, name)) return g_rails[i];
    }
    return NULL;
}

// --- The states ---
typedef struct WheelState {
    float comp, comp_prev;   // m
    float omega, spin;       // rad/s, rad
    float steer;             // rad
    float load, slip;        // N, the slip (for the tests and sounds)
    bool down;
    int partner;             // the other wheel of its axle (anti-roll), -1
} WheelState;
typedef struct VState {
    Entity e;
    const VehicleAsset* a;
    uint32_t version;
    Vec x, v, w;             // m (the center of mass), m/s, rad/s
    Quat q;
    float inv_i[3];          // the body's diagonal inverse inertia
    WheelState wh[VEH_MAX_PARTS];
    float part_t[VEH_MAX_PARTS], part_goal[VEH_MAX_PARTS]; // hinges, sliders, pistons: 0..1 now, wanted
    float part_spin[VEH_MAX_PARTS];                         // gears, propellers, rotors: their angle (rad)
    float part_base[VEH_MAX_PARTS][6];                      // their components' rest place and rotation
    Entity part_ent[VEH_MAX_PARTS];
    VehControls in, player;  // the Blueprint's, the player's
    bool player_drives, engine_on, manual;
    uint8_t keys_prev;
    int gear;                // -1 reverse, 0 neutral, 1..
    float rpm, shift_t, throttle_lever, collective_lever;
    float cam_yaw, cam_yaw_off;
    // a train
    RailPath* rail;
    float rail_s, rail_v;
    struct VState* leader;
    float submerged;
    uint32_t contacts;
    Entity driver;           // the pawn that entered it (exit: back to it)
    // (Q27.4) damage: the dents (cm, inward) on a grid over its box (8 along, 4 across, 4 up), what they did to it: the
    // engine's power lost (0..0.8), each front wheel's toe (rad: the car pulls), the whole (0..1), the hits taken
    float dent[8][4][4];
    float dmg_engine, dmg_toe[2], damage;
    uint32_t hits;
} VState;

static struct {
    VState* s[VS_MAX];
    uint32_t n;
    VehStats stats;
    bool playing;
} VS;

static VState* state_of(Entity e) {
    for (uint32_t i = 0; i < VS.n; i++)
        if (VS.s[i]->e == e) return VS.s[i];
    return NULL;
}

static int part_named(const VehicleAsset* a, const char* name) {
    for (uint32_t i = 0; i < a->nparts; i++)
        if (!_stricmp(a->parts[i].name, name)) return (int)i;
    return -1;
}

static Entity comp_named(World* w, Entity owner, const char* name) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* ar = &w->archetypes[ai];
        if (!(ar->mask & COMP_BIT(COMP_ATTACH)) || !(ar->mask & COMP_BIT(COMP_NAME)) || !ar->count) continue;
        const CAttach* at = ecs_arch_column(ar, COMP_ATTACH);
        const CName* nm = ecs_arch_column(ar, COMP_NAME);
        const Entity* ents = ecs_arch_entities(ar);
        for (uint32_t r = 0; r < ar->count; r++)
            if (at[r].owner == owner && at[r].kind == ATTACH_SCS && !strcmp(nm[r].text, name)) return ents[r];
    }
    return ENTITY_NONE;
}

// A vehicle's state made (or remade when its asset changed): where its actor is, at rest
static void state_start(World* w, VState* s, const VehicleAsset* a) {
    const CTransform* t = ecs_get(w, s->e, COMP_TRANSFORM);
    memset(&s->x, 0, sizeof *s - offsetof(VState, x));
    s->a = a, s->version = a->version;
    Mat r = rot_mat(t->rotation);
    s->q = matq(&r);
    s->x = vadd(vmul(vof(t->location), 0.01f), mv(&r, vmul(vof(a->com), 0.01f)));
    float m = fmaxf(a->mass, 1.0f), ex = a->extent[0] * 0.01f, ey = a->extent[1] * 0.01f, ez = a->extent[2] * 0.01f;
    s->inv_i[0] = 3.0f / (m * (ey * ey + ez * ez) + 1e-3f), s->inv_i[1] = 3.0f / (m * (ex * ex + ez * ez) + 1e-3f);
    s->inv_i[2] = 3.0f / (m * (ex * ex + ey * ey) + 1e-3f);
    const CVehicle* cv = ecs_get(w, s->e, COMP_VEHICLE);
    s->engine_on = cv && cv->start_engine, s->gear = 1, s->rpm = 0;
    for (uint32_t i = 0; i < a->nparts; i++) {
        const VehPart* p = &a->parts[i];
        s->wh[i].partner = -1;
        if (p->kind == VP_WHEEL)
            for (uint32_t j = 0; j < a->nparts; j++)
                if (j != i && a->parts[j].kind == VP_WHEEL && fabsf(a->parts[j].pos[0] - p->pos[0]) < 1.0f && a->parts[j].pos[1] * p->pos[1] < 0.0f) s->wh[i].partner = (int)j;
        bool open = (p->kind == VP_HINGE || p->kind == VP_SLIDER) && p->p[3] > 0.5f;
        s->part_t[i] = s->part_goal[i] = open ? 1.0f : 0.0f;
        s->part_ent[i] = p->attach[0] ? comp_named(w, s->e, p->attach) : ENTITY_NONE;
        const CAttach* at = s->part_ent[i] ? ecs_get(w, s->part_ent[i], COMP_ATTACH) : NULL;
        if (at) memcpy(s->part_base[i], at->location, 12), memcpy(s->part_base[i] + 3, at->rotation, 12);
    }
    // the gearbox
    s->manual = false;
    for (uint32_t i = 0; i < a->nparts; i++)
        if (a->parts[i].kind == VP_GEARBOX) s->manual = a->parts[i].p[1] < 0.5f;
    // a train: on its rail
    if (cv && cv->rail[0]) {
        s->rail = rail_named(w, cv->rail);
        s->rail_s = cv->rail_start, s->rail_v = 0.0f;
    }
    s->cam_yaw = t->rotation[1];
}

// --- The pieces of the drivetrain ---
static float engine_torque(const VehPart* e, float rpm) { // N m at full throttle
    if (rpm > e->p[2]) return 0.0f; // (the limiter)
    float x = (rpm - e->p[1]) / fmaxf(e->p[1], 1.0f);
    return e->p[0] * clampf(1.0f - 0.6f * x * x, 0.3f, 1.0f);
}
static float magic(float x, float b, float c) { return sinf(c * atanf(b * x)); } // (Pacejka's, D = 1, E = 0)

// --- One substep of a vehicle ---
static void step(World* w, VState* s, float h, const VehControls* in, float (*boxes)[6], uint32_t nb) {
    const VehicleAsset* a = s->a;
    float m = fmaxf(a->mass, 1.0f);
    Mat R = qmat(s->q);
    Vec up = mv(&R, vv(0, 0, 1)), fwd = mv(&R, vv(1, 0, 0)), left = mv(&R, vv(0, 1, 0));
    Vec F = vv(0, 0, -9.81f * m), T = vv(0, 0, 0);
    Vec com = vmul(vof(a->com), 0.01f);
    float speed_f = vdot(s->v, fwd);
    const VehPart *eng = NULL, *box = NULL, *dif = NULL;
    for (uint32_t i = 0; i < a->nparts; i++) {
        if (a->parts[i].kind == VP_ENGINE) eng = &a->parts[i];
        if (a->parts[i].kind == VP_GEARBOX) box = &a->parts[i];
        if (a->parts[i].kind == VP_DIFFERENTIAL) dif = &a->parts[i];
    }
    // throttle, brake and the gear from the controls (the automatic's reverse: hold brake when stopped)
    // a negative throttle (S) brakes while rolling forward and backs up once stopped (the automatic); Brake only brakes
    float throttle = clampf(in->throttle, -1.0f, 1.0f), brake = clampf(in->brake, 0.0f, 1.0f);
    float drive_any = 0.0f;
    for (uint32_t i = 0; i < a->nparts; i++)
        if (a->parts[i].kind == VP_WHEEL) drive_any += a->parts[i].p[6];
    if (drive_any > 0.0f && !s->manual && s->engine_on) {
        if (throttle < -0.05f && s->gear >= 0 && speed_f < 0.5f) s->gear = -1;
        if (throttle > 0.05f && s->gear < 0 && speed_f > -0.5f) s->gear = 1;
        if (s->gear >= 0 && throttle < 0.0f) brake = fmaxf(brake, -throttle), throttle = 0.0f;
        else if (s->gear < 0 && throttle > 0.0f) brake = fmaxf(brake, throttle), throttle = 0.0f;
        else throttle = fabsf(throttle);
    }
    float reverse_prop = throttle < 0.0f ? -throttle : 0.0f; // (a boat's S: its propeller backward)
    throttle = fmaxf(throttle, 0.0f);
    if (!s->engine_on) throttle = 0.0f;
    if (a->max_speed > 0.0f && fabsf(speed_f) * 3.6f > a->max_speed) throttle = 0.0f;
    // the engine's rpm from the drive wheels, the clutch slipping below idle
    float ratio = 0.0f, final = box ? box->p[0] : 1.0f;
    if (s->gear > 0 && a->ngears) ratio = a->gears[(s->gear - 1) < (int)a->ngears ? s->gear - 1 : (int)a->ngears - 1];
    else if (s->gear < 0) ratio = -a->reverse;
    float drive_sum = 0.0f, omega_drive = 0.0f;
    for (uint32_t i = 0; i < a->nparts; i++)
        if (a->parts[i].kind == VP_WHEEL && a->parts[i].p[6] > 0.0f) drive_sum += a->parts[i].p[6], omega_drive += s->wh[i].omega * a->parts[i].p[6];
    if (drive_sum > 0.0f) omega_drive /= drive_sum;
    float t_wheels = 0.0f;
    if (eng && drive_sum > 0.0f) {
        float idle = eng->p[3], rpm_w = fabsf(omega_drive * ratio * final) * 60.0f / (2.0f * VS_PI);
        bool shifting = s->shift_t > 0.0f;
        if (ratio == 0.0f || shifting) s->rpm += ((idle + throttle * (eng->p[2] - idle) * 0.8f) - s->rpm) * clampf(h * 4.0f, 0, 1);
        else s->rpm = fmaxf(rpm_w, idle + throttle * (eng->p[1] - idle) * 0.5f * (rpm_w < idle * 1.5f)); // (the clutch slips)
        if (!s->engine_on) s->rpm = 0.0f;
        float te = s->engine_on ? engine_torque(eng, s->rpm) * throttle * (1.0f - s->dmg_engine) : 0.0f; // (Q27.4: a crushed front)
        if (throttle < 0.05f && s->rpm > idle * 1.2f && ratio != 0.0f) te = -eng->p[0] * eng->p[4] * (s->rpm / eng->p[2]); // (engine braking)
        t_wheels = shifting ? 0.0f : te * ratio * final * 0.9f;
        // the automatic's shifts
        if (box && !s->manual && s->gear > 0 && !shifting) {
            // (by the wheels' rpm, not the engine's: a slipping clutch or a wheelspin doesn't shift)
            if (rpm_w > box->p[2] && s->gear < (int)a->ngears) s->gear++, s->shift_t = box->p[4];
            else if (rpm_w < box->p[3] && s->gear > 1) s->gear--, s->shift_t = box->p[4] * 0.6f;
        }
        if (s->shift_t > 0.0f) s->shift_t -= h;
    }
    // --- the wheels ---
    float diff_lock = dif ? (dif->p[0] > 1.5f ? dif->p[1] : dif->p[0] > 0.5f ? 1.0f : 0.0f) : 0.0f;
    float steer = clampf(in->steer, -1.0f, 1.0f);
    for (uint32_t i = 0; i < a->nparts; i++) {
        const VehPart* p = &a->parts[i];
        if (p->kind != VP_WHEEL) continue;
        WheelState* ws = &s->wh[i];
        float rr = p->p[0] * 0.01f, L = p->p[4] * 0.01f;
        Vec mount = vadd(s->x, mv(&R, vsub(vmul(vof(p->pos), 0.01f), com)));
        Vec end = vsub(mount, vmul(up, L + rr));
        float ac[3] = { mount.x * 100.0f, mount.y * 100.0f, mount.z * 100.0f }, bc[3] = { end.x * 100.0f, end.y * 100.0f, end.z * 100.0f };
        float f = a->kind == VK_TRAIN ? 1.0f : g_trace(ac, bc, 0.0f);
        ws->comp_prev = ws->comp;
        ws->down = f < 1.0f;
        float dist = f * (L + rr);
        ws->comp = ws->down ? clampf(L + rr - dist, 0.0f, L) : 0.0f;
        // the steering (less at speed: stable at 200 km/h, tight at a walk)
        float st = p->p[5] * (VS_PI / 180.0f) * steer / (1.0f + fabsf(speed_f) / 25.0f);
        if (p->p[5] > 0.0f) st += s->dmg_toe[p->pos[1] > 0.0f ? 1 : 0]; // (Q27.4: a bent front wheel's toe: it pulls)
        ws->steer += (st - ws->steer) * clampf(h * 10.0f, 0, 1);
        // the drive torque: its share (a tank: each side by the throttle and the steering)
        float td = drive_sum > 0.0f ? t_wheels * p->p[6] / drive_sum : 0.0f;
        if (a->kind == VK_TANK && eng && p->p[6] > 0.0f) {
            float side = p->pos[1] > 0.0f ? 1.0f : -1.0f; // (+y: the right track; turning right, the left one faster)
            float cmd = clampf(throttle * (s->gear < 0 ? -1.0f : 1.0f) - steer * side, -1.0f, 1.0f);
            td = engine_torque(eng, fmaxf(s->rpm, eng->p[1])) * cmd * a->gears[0] * final * 0.9f * p->p[6] / drive_sum;
            if (!s->engine_on) td = 0.0f;
        }
        float mw = fmaxf(15.0f, m * 0.02f / 4.0f), iw = 0.5f * mw * rr * rr;
        if (!ws->down) { // in the air: it spins up by its drive, slows by its brake
            ws->omega += td / iw * h;
            float tb = p->p[7] * brake + p->p[8] * in->handbrake;
            float dw = tb / iw * h;
            ws->omega = fabsf(ws->omega) <= dw ? 0.0f : ws->omega - copysignf(dw, ws->omega);
            ws->load = 0.0f, ws->slip = 0.0f;
            ws->spin += ws->omega * h;
            continue;
        }
        // the spring and the damper (and the anti-roll bar: half the spring between the two wheels of the axle)
        float k = p->p[2] * 100.0f, c = p->p[3] * 100.0f;
        float fs = k * ws->comp + c * (ws->comp - ws->comp_prev) / h;
        if (ws->partner >= 0) fs += 0.5f * k * (ws->comp - s->wh[ws->partner].comp);
        if (ws->comp >= L * 0.999f) fs += m * 9.81f * 0.5f; // (bottomed out: the bump stop)
        fs = fmaxf(fs, 0.0f);
        Vec contact = vsub(mount, vmul(up, dist));
        Vec r = vsub(contact, s->x);
        float cs = cosf(ws->steer), sn = sinf(ws->steer);
        Vec wf = vadd(vmul(fwd, cs), vmul(left, sn)), wl = vsub(vmul(left, cs), vmul(fwd, sn));
        Vec vc = vadd(s->v, vcross(s->w, r));
        float vf = vdot(vc, wf), vl = vdot(vc, wl);
        float mu = p->p[9], fz = fs;
        // traction control and ABS: the drive and the brakes no stronger than the tire's peak grip (it spins and locks no
        // further than where it grips best); the handbrake still locks the wheel (drifts)
        float grip_t = mu * fz * rr;
        td = clampf(td, -grip_t * 1.05f, grip_t * 1.05f);
        // the wheel's spin first, implicit against the tire's slope (the wheel is stiff: explicitly it would leap past the
        // tire's peak in one substep and spin away): om (1 + h/I Ck r^2/d) = om0 + h/I (Td + Ck r vf/d)
        float d = fmaxf(fabsf(vf), 1.0f), ck = mu * fz * 19.0f, ai = h / iw;
        ws->omega = (ws->omega + ai * (td + ck * rr * vf / d)) / (1.0f + ai * ck * rr * rr / d);
        // the tire: longitudinal by the slip ratio, lateral by the slip angle (a magic formula each), the friction ellipse
        float kappa = (ws->omega * rr - vf) / d;
        float alpha = atan2f(vl, fmaxf(fabsf(vf), 1.5f));
        float fx = mu * fz * magic(kappa, 10.0f, 1.9f), fy = -mu * fz * magic(alpha, 8.0f, 1.3f);
        if (a->kind == VK_TANK) fy *= 0.3f; // (tracks slide sideways as they skid-steer)
        float ft = sqrtf(fx * fx + fy * fy), fmax = mu * fz;
        if (ft > fmax && ft > 0.0f) fx *= fmax / ft, fy *= fmax / ft;
        // the brakes (ABS: no stronger than the grip; the handbrake locks)
        float tb = fminf(p->p[7] * brake, grip_t * 1.1f) + p->p[8] * in->handbrake;
        float dw = tb / iw * h;
        if (dw > 0.0f) ws->omega = fabsf(ws->omega) <= dw ? 0.0f : ws->omega - copysignf(dw, ws->omega);
        ws->spin += ws->omega * h;
        ws->load = fz, ws->slip = kappa;
        Vec force = vadd(vadd(vmul(up, fs), vmul(wf, fx)), vmul(wl, fy));
        F = vadd(F, force), T = vadd(T, vcross(r, force));
    }
    // the differential: the drive wheels' speeds pulled together (locked: all the way)
    if (diff_lock > 0.0f && drive_sum > 0.0f) {
        float mean = 0.0f;
        for (uint32_t i = 0; i < a->nparts; i++)
            if (a->parts[i].kind == VP_WHEEL && a->parts[i].p[6] > 0.0f) mean += s->wh[i].omega * a->parts[i].p[6];
        mean /= drive_sum;
        if (a->kind != VK_TANK)
            for (uint32_t i = 0; i < a->nparts; i++)
                if (a->parts[i].kind == VP_WHEEL && a->parts[i].p[6] > 0.0f) s->wh[i].omega += (mean - s->wh[i].omega) * diff_lock * 0.5f;
    }
    // --- floats, propellers, wings, rotors ---
    float submerged = 0.0f;
    uint32_t nfloat = 0;
    for (uint32_t i = 0; i < a->nparts; i++) {
        const VehPart* p = &a->parts[i];
        Vec pw = vadd(s->x, mv(&R, vsub(vmul(vof(p->pos), 0.01f), com)));
        Vec r = vsub(pw, s->x);
        Vec vp = vadd(s->v, vcross(s->w, r));
        float wz;
        const WaterBody* wb = g_nwater ? water_at(pw.x, pw.y, &wz) : NULL;
        switch (p->kind) {
        case VP_FLOAT: {
            nfloat++;
            if (!wb) break;
            float rad = p->p[0] * 0.01f, depth = wz - (pw.z - rad), frac = clampf(depth / (2.0f * rad), 0.0f, 1.0f);
            if (frac <= 0.0f) break;
            submerged += frac;
            float vol = 4.0f / 3.0f * VS_PI * rad * rad * rad;
            Vec rel = vsub(vp, vv(wb->cur[0], wb->cur[1], 0.0f));
            Vec force = vv(0, 0, wb->rho * 9.81f * vol * frac);
            // the water's drag: quadratic (its area) and a little linear (bobbing settles)
            float area = VS_PI * rad * rad;
            force = vsub(force, vmul(rel, 0.5f * wb->rho * area * p->p[1] * frac * vlen(rel) * 0.05f));
            force.z -= rel.z * frac * m * 0.6f / 8.0f;
            F = vadd(F, force), T = vadd(T, vcross(r, force));
            break;
        }
        case VP_PROPELLER: {
            bool water = p->p[3] > 0.5f;
            if (water && (!wb || pw.z > wz)) break;
            float thr = a->kind == VK_AIRPLANE ? s->throttle_lever : throttle - (brake + reverse_prop) * p->p[1];
            if (!s->engine_on) thr = 0.0f;
            Vec dir = mv(&R, vnorm(vof(p->axis)));
            // (thrust falls as the craft nears the propeller's own speed)
            float vin = vdot(vp, dir), vmax = water ? 18.0f : 90.0f;
            Vec force = vmul(dir, p->p[0] * thr * clampf(1.0f - vin / vmax, 0.0f, 1.2f));
            F = vadd(F, force), T = vadd(T, vcross(r, force));
            s->part_spin[i] += thr * p->p[2] / 60.0f * 2.0f * VS_PI * h;
            break;
        }
        case VP_WING: {
            bool water = p->p[6] > 0.5f;
            float rho = water ? (wb ? wb->rho : 1000.0f) : 1.225f;
            if (water && (!wb || pw.z > wz)) break;
            Vec rel = water && wb ? vsub(vp, vv(wb->cur[0], wb->cur[1], 0.0f)) : vp;
            float sp = vlen(rel);
            if (sp < 0.3f) break;
            Vec n = mv(&R, vnorm(vof(p->axis)));
            Vec chord = vnorm(vsub(fwd, vmul(n, vdot(fwd, n))));
            float u = vdot(rel, chord), vn = vdot(rel, n);
            float alpha = atan2f(-vn, fabsf(u) > 1e-4f ? fabsf(u) : 1e-4f);
            float ctrl = 0.0f;
            switch (p->input) {
            case VI_PITCH: ctrl = -in->pitch; break;
            case VI_ROLL: ctrl = in->roll * (p->pos[1] >= 0.0f ? -1.0f : 1.0f); break;
            case VI_YAW: case VI_STEER: ctrl = -(p->input == VI_YAW ? in->yaw : steer); break;
            default: break;
            }
            float stall = p->p[2] * (VS_PI / 180.0f), ae = alpha + ctrl * p->p[4] * (VS_PI / 180.0f);
            float cl = fabsf(ae) <= stall ? p->p[1] * ae : copysignf(p->p[1] * stall * fmaxf(0.35f, 1.0f - (fabsf(ae) - stall) / (stall * 2.0f)), ae);
            float qd = 0.5f * rho * sp * sp * p->p[0];
            Vec vh = vmul(rel, 1.0f / sp);
            Vec ldir = vsub(n, vmul(vh, vdot(n, vh)));
            float ll = vlen(ldir);
            Vec force = ll > 1e-4f ? vmul(ldir, qd * cl / ll) : vv(0, 0, 0);
            float cd = p->p[3] + cl * cl / (VS_PI * fmaxf(p->p[5], 1.0f) * 0.8f);
            force = vsub(force, vmul(vh, qd * cd));
            F = vadd(F, force), T = vadd(T, vcross(r, force));
            break;
        }
        case VP_ROTOR: {
            if (!s->engine_on) break;
            float maxl = fmaxf(p->p[0], 1.0f), stab = p->p[3];
            float hover = clampf(m * 9.81f / maxl, 0.0f, 1.0f);
            // the collective: the lever, around a hover when Stability helps (and holding the height when let go)
            float col = stab > 0.0f ? hover + s->collective_lever * (1.0f - hover) * 0.9f - s->v.z * 0.08f * stab : s->collective_lever;
            col = clampf(col, 0.0f, 1.0f);
            float cyc = p->p[1] * (VS_PI / 180.0f);
            Vec dir = vnorm(vadd(up, vadd(vmul(fwd, -tanf(in->pitch * cyc)), vmul(left, tanf(in->roll * cyc))))); // (left: the local +y, Unreal's right)
            Vec force = vmul(dir, col * maxl);
            F = vadd(F, force), T = vadd(T, vcross(r, force));
            // the tail: the yaw; Stability: levels it and damps its turns
            T = vadd(T, vmul(up, in->yaw * p->p[2] - vdot(s->w, up) * p->p[2] * 0.3f));
            if (stab > 0.0f) {
                Vec lev = vcross(up, vv(0, 0, 1));
                T = vadd(T, vmul(lev, stab * m * 9.81f * 1.2f));
                T = vsub(T, vmul(vsub(s->w, vmul(up, vdot(s->w, up))), stab * m * 1.5f));
            }
            s->part_spin[i] += p->p[4] / 60.0f * 2.0f * VS_PI * h * (0.3f + col);
            break;
        }
        default: break;
        }
    }
    s->submerged = nfloat ? submerged / (float)nfloat : 0.0f;
    // a motorcycle stays up: it leans into its turn (the lean its speed and turn need) and rights itself
    if (a->kind == VK_MOTORCYCLE) {
        float yawrate = vdot(s->w, up), lean_goal = clampf(atanf(speed_f * yawrate / 9.81f), -0.8f, 0.8f);
        float lean = asinf(clampf(-vdot(left, vv(0, 0, 1)), -1.0f, 1.0f));
        float rollrate = vdot(s->w, fwd);
        T = vadd(T, vmul(fwd, -(m * 9.81f * 0.6f) * 6.0f * (lean_goal - lean) - m * 0.8f * rollrate));
    }
    // a tank turns on the spot as the tracks would (its steering's yaw, damped)
    if (a->kind == VK_TANK && s->engine_on) {
        uint32_t down = 0;
        for (uint32_t i = 0; i < a->nparts; i++) down += a->parts[i].kind == VP_WHEEL && s->wh[i].down;
        if (down) T = vsub(T, vmul(up, vdot(s->w, up) * m * 6.0f)); // (the tracks' turn resisted: about 40 degrees a second)
    }
    // the air: drag, downforce
    float sp = vlen(s->v);
    F = vsub(F, vmul(s->v, 0.5f * 1.225f * a->drag * sp));
    F = vsub(F, vmul(up, a->downforce * speed_f * speed_f));
    // integrate (the inertia in the world from the body's diagonal)
    s->v = vadd(s->v, vmul(F, h / m));
    Vec tl = mtv(&R, T), wl = mtv(&R, s->w);
    float ix = 1.0f / s->inv_i[0], iy = 1.0f / s->inv_i[1], iz = 1.0f / s->inv_i[2];
    Vec gyro = vcross(wl, vv(ix * wl.x, iy * wl.y, iz * wl.z));
    wl = vadd(wl, vv((tl.x - gyro.x) * s->inv_i[0] * h, (tl.y - gyro.y) * s->inv_i[1] * h, (tl.z - gyro.z) * s->inv_i[2] * h));
    s->w = mv(&R, wl);
    s->w = vmul(s->w, 1.0f - clampf(h * (s->submerged > 0.0f ? 1.5f : 0.05f), 0.0f, 0.5f));
    s->x = vadd(s->x, vmul(s->v, h));
    Quat dq = { s->w.x * h * 0.5f, s->w.y * h * 0.5f, s->w.z * h * 0.5f, 0.0f };
    Quat q = s->q;
    s->q = qn((Quat){ q.x + dq.w * q.x + dq.x * q.w + dq.y * q.z - dq.z * q.y, q.y + dq.w * q.y - dq.x * q.z + dq.y * q.w + dq.z * q.x,
                      q.z + dq.w * q.z + dq.x * q.y - dq.y * q.x + dq.z * q.w, q.w + dq.w * q.w - dq.x * q.x - dq.y * q.y - dq.z * q.z });
    // the chassis against the world: its box's corners and bottom (impulses with friction, pushed out)
    R = qmat(s->q);
    float ex = a->extent[0] * 0.01f, ey = a->extent[1] * 0.01f, ez = a->extent[2] * 0.01f;
    static const float pts[13][3] = { { 1, 1, 1 }, { 1, -1, 1 }, { -1, 1, 1 }, { -1, -1, 1 }, { 1, 1, -1 }, { 1, -1, -1 }, { -1, 1, -1 }, { -1, -1, -1 },
                                      { 0, 0, -1 }, { 1, 0, -1 }, { -1, 0, -1 }, { 0, 1, -1 }, { 0, -1, -1 } };
    for (int k = 0; k < 13; k++) {
        Vec lp = vsub(vv(pts[k][0] * ex, pts[k][1] * ey, pts[k][2] * ez), com);
        Vec r = mv(&R, lp), P = vadd(s->x, r);
        Vec n = vv(0, 0, 1);
        float depth = -1.0f;
        float gz;
        if (g_flat_ground) { if (P.z < 0.0f) depth = -P.z; }
        else if (landscape_height(P.x * 100.0f, P.y * 100.0f, &gz) && P.z * 100.0f < gz) depth = gz * 0.01f - P.z;
        for (uint32_t b = 0; b < nb && !g_flat_ground; b++) {
            const float* bx = boxes[b];
            float pc[3] = { P.x * 100.0f, P.y * 100.0f, P.z * 100.0f };
            if (pc[0] < bx[0] || pc[0] > bx[3] || pc[1] < bx[1] || pc[1] > bx[4] || pc[2] < bx[2] || pc[2] > bx[5]) continue;
            float pen[6] = { pc[0] - bx[0], bx[3] - pc[0], pc[1] - bx[1], bx[4] - pc[1], pc[2] - bx[2], bx[5] - pc[2] };
            static const Vec nn[6] = { { -1, 0, 0 }, { 1, 0, 0 }, { 0, -1, 0 }, { 0, 1, 0 }, { 0, 0, -1 }, { 0, 0, 1 } };
            int best = 0;
            for (int j = 1; j < 6; j++)
                if (pen[j] < pen[best]) best = j;
            if (pen[best] * 0.01f > depth) depth = pen[best] * 0.01f, n = nn[best];
        }
        if (depth <= 0.0f) continue;
        s->contacts++;
        s->x = vadd(s->x, vmul(n, fminf(depth, 0.05f) * 0.5f));
        Vec vpt = vadd(s->v, vcross(s->w, r));
        float vn = vdot(vpt, n);
        if (vn >= 0.0f) continue;
        Vec ln = mtv(&R, vcross(r, n));
        Vec iln = mv(&R, vv(ln.x * s->inv_i[0], ln.y * s->inv_i[1], ln.z * s->inv_i[2]));
        float kn = 1.0f / m + vdot(n, vcross(iln, r));
        float j = -(1.0f + 0.15f) * vn / kn;
        Vec vt = vsub(vpt, vmul(n, vn));
        float vtl = vlen(vt);
        Vec imp = vmul(n, j);
        if (vtl > 1e-4f) imp = vsub(imp, vmul(vt, fminf(0.6f * j, vtl * m * 0.5f) / vtl)); // (friction)
        s->v = vadd(s->v, vmul(imp, 1.0f / m));
        Vec li = mtv(&R, vcross(r, imp));
        s->w = vadd(s->w, mv(&R, vv(li.x * s->inv_i[0], li.y * s->inv_i[1], li.z * s->inv_i[2])));
    }
    (void)w;
}
