#include "lens_water.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

// A drop on the lens: where (screen heights), how big, how fast it runs, whether it holds, its trail
typedef struct LensDrop {
    float p[2], v[2], r;
    float alpha;                      // fades in as it lands, out as it dries
    float flake;                      // (2026-11-15) 1: a snowflake stuck on the lens, melting to 0 (a drop)
    bool moving;
    float trail[LENS_TRAIL][3];       // x, y, age (s); trail[0] the newest
    uint8_t ntrail;
} LensDrop;

static struct {
    LensDrop d[LENS_MAX];
    uint32_t n;
    float spawn_acc;
    uint32_t rng;
    bool have_cam;
    float last_pos[3], last_vel[3], last_fwd[3], last_right[3], last_up[3];
    float omega[2];   // the camera's turn (yaw, pitch: rad/s), smoothed
    float aspect;
    float trail_life; // s a trail lasts (packing: its alpha)
} L = { .rng = 0x9E3779B9u };

static float rnd(void) {
    L.rng ^= L.rng << 13, L.rng ^= L.rng >> 17, L.rng ^= L.rng << 5;
    return (float)(L.rng >> 8) * (1.0f / 16777216.0f);
}
static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static float clampf(float x, float a, float b) { return x < a ? a : x > b ? b : x; }

void lens_reset(void) {
    L.n = 0, L.spawn_acc = 0.0f, L.have_cam = false, L.omega[0] = L.omega[1] = 0.0f;
}
uint32_t lens_drop_count(void) { return L.n; }

static void drop_remove(uint32_t i) { L.d[i] = L.d[--L.n]; }

static void drop_add(float x, float y, float r) {
    uint32_t i = L.n;
    if (L.n == LENS_MAX) { // full: the smallest holding drop makes room
        float best = 1e9f;
        for (uint32_t k = 0; k < L.n; k++)
            if (!L.d[k].moving && L.d[k].r < best) best = L.d[k].r, i = k;
        if (i == LENS_MAX) return;
    } else
        L.n++;
    LensDrop* d = &L.d[i];
    memset(d, 0, sizeof *d);
    d->p[0] = x, d->p[1] = y, d->r = r, d->alpha = 0.0f;
}

void lens_step(const LensParams* p, const LensCamera* c, float arrive, bool drying, bool snow, float dt) {
    if (dt <= 0.0f) return;
    if (dt > 0.1f) dt = 0.1f;
    float asp = c->aspect > 0.1f ? c->aspect : 16.0f / 9.0f;
    L.aspect = asp;
    float size = clampf(p->size, 0.3f, 3.0f);
    const float G = 0.9f; // (screen heights / s^2: a big drop runs down the view in about a second)
    float r_ref = 0.011f * size * clampf(p->stickiness, 0.05f, 3.0f); // (smaller than this holds under gravity alone)
    L.trail_life = 1.6f * clampf(p->trail, 0.0f, 2.0f);
    // the camera: its acceleration and its turn, as forces on the drops (they lag behind what the lens does)
    float acc_s[2] = { 0.0f, 0.0f }, fwd_speed = 0.0f, shake = 0.0f;
    if (L.have_cam) {
        float vel[3], acc[3];
        for (int k = 0; k < 3; k++) vel[k] = (c->pos[k] - L.last_pos[k]) / dt;
        float jump = sqrtf(dot3(vel, vel)) * dt, turn = 1.0f - dot3(c->fwd, L.last_fwd);
        if (jump > 500.0f || turn > 0.5f) { // a cut (a teleport, a camera switched): no force from it
            memcpy(L.last_vel, vel, sizeof vel);
            L.omega[0] = L.omega[1] = 0.0f;
        } else {
            for (int k = 0; k < 3; k++) acc[k] = (vel[k] - L.last_vel[k]) / dt / 981.0f, L.last_vel[k] = vel[k]; // (in g)
            float df[3] = { c->fwd[0] - L.last_fwd[0], c->fwd[1] - L.last_fwd[1], c->fwd[2] - L.last_fwd[2] };
            float om[2] = { dot3(df, L.last_right) / dt, dot3(df, L.last_up) / dt }; // (rad/s: turning right, up)
            float al[2] = { (om[0] - L.omega[0]) / dt, (om[1] - L.omega[1]) / dt };
            L.omega[0] = om[0], L.omega[1] = om[1];
            float in = clampf(p->inertia, 0.0f, 3.0f);
            // the lens moving right pushes them left on the screen; up, down (the screen's y goes down)
            float ax = dot3(acc, c->right), ay = -dot3(acc, c->up);
            acc_s[0] = -(ax * G + al[0] * 0.05f) * in;
            acc_s[1] = -(ay * G - al[1] * 0.05f) * in;
            // a turn swings them out too (the lens is ahead of the axis it turns about)
            float w2 = om[0] * om[0] + om[1] * om[1];
            acc_s[0] += -om[0] * fabsf(om[0]) * 0.02f * in, acc_s[1] += om[1] * fabsf(om[1]) * 0.02f * in;
            fwd_speed = dot3(vel, c->fwd) * 0.01f; // m/s
            shake = sqrtf(acc_s[0] * acc_s[0] + acc_s[1] * acc_s[1]) + sqrtf(w2) * 0.8f * in;
        }
    }
    L.have_cam = true;
    memcpy(L.last_pos, c->pos, sizeof L.last_pos), memcpy(L.last_fwd, c->fwd, sizeof L.last_fwd);
    memcpy(L.last_right, c->right, sizeof L.last_right), memcpy(L.last_up, c->up, sizeof L.last_up);
    // gravity on the lens: the world's down seen on the screen (looking level: straight down the view; tilted: aslant;
    // looking straight down or up: almost nothing, the drops stay)
    float gs[2] = { -c->right[2] * G * p->gravity, c->up[2] * G * p->gravity };
    // the drops arriving (the size: mostly small, a few big)
    if (!drying && arrive > 0.0f) {
        L.spawn_acc += arrive * clampf(p->amount, 0.0f, 2.0f) * dt;
        while (L.spawn_acc >= 1.0f) {
            L.spawn_acc -= 1.0f;
            float u = rnd();
            if (snow) { // a flake: bigger, it sticks and melts into a smaller drop
                drop_add(rnd() * asp, rnd(), size * (0.008f + 0.012f * u));
                if (L.n) L.d[L.n - 1].flake = 1.0f;
            } else
                drop_add(rnd() * asp, rnd(), size * (0.004f + 0.011f * u * u));
        }
    } else
        L.spawn_acc = 0.0f;
    float air = clampf(p->airflow, 0.0f, 3.0f) * (fwd_speed > 0.0f ? fwd_speed * fwd_speed : 0.0f) * 0.004f;
    float shake_thr = p->shake_off > 0.0f ? 7.0f / p->shake_off : 1e9f;
    float dry = drying ? 0.012f * size / fmaxf(p->dry_time, 0.1f) : 0.0f; // (the biggest drop gone in Dry Time)
    for (uint32_t i = 0; i < L.n;) {
        LensDrop* d = &L.d[i];
        d->alpha = fminf(d->alpha + dt * 12.0f, 1.0f);
        if (d->flake > 0.0f) { // melting (~4 s; faster out of the cold): the water it leaves is a smaller drop
            float m = fminf(d->flake, dt / (drying ? 2.0f : 4.0f));
            d->flake -= m, d->r *= 1.0f - m * 0.5f;
        }
        for (uint32_t k = 0; k < d->ntrail; k++) d->trail[k][2] += dt;
        while (d->ntrail && d->trail[d->ntrail - 1][2] > L.trail_life) d->ntrail--;
        // the forces along the lens (per unit mass: screen heights / s^2)
        float F[2] = { gs[0] + acc_s[0], gs[1] + acc_s[1] };
        if (air > 0.0f) { // moving forward: the air pushes them out from the middle of the view
            float ox = d->p[0] - asp * 0.5f, oy = d->p[1] - 0.5f, ol = sqrtf(ox * ox + oy * oy) + 1e-3f;
            F[0] += ox / ol * air, F[1] += oy / ol * air;
        }
        float Fl = sqrtf(F[0] * F[0] + F[1] * F[1]);
        float hold = G * (r_ref / d->r) * (r_ref / d->r); // (what its contact holds: less for a big drop)
        // a quick move throws it off the lens (the bigger, the easier)
        if (shake > shake_thr * (0.6f + 0.4f * r_ref / d->r) && rnd() < dt * 12.0f * (shake / shake_thr - 0.5f)) {
            drop_remove(i);
            continue;
        }
        if (!d->moving && Fl > hold && d->flake < 0.3f) d->moving = true; // (a flake holds until it is mostly water)
        if (d->moving) {
            float vl = sqrtf(d->v[0] * d->v[0] + d->v[1] * d->v[1]);
            float fx = F[0], fy = F[1];
            if (vl > 1e-4f) fx -= d->v[0] / vl * hold * 0.6f, fy -= d->v[1] / vl * hold * 0.6f; // (sliding: a smaller friction)
            d->v[0] += fx * dt, d->v[1] += fy * dt;
            float damp = expf(-dt * 3.0f); // (the water's drag on the glass)
            d->v[0] *= damp, d->v[1] *= damp;
            // a wandering path: the glass's specks nudge it across
            float ph = d->p[1] * 37.0f + d->p[0] * 11.0f;
            d->v[0] += sinf(ph) * 0.15f * vl * dt * 6.0f, d->v[1] += cosf(ph * 0.7f) * 0.05f * vl * dt * 6.0f;
            d->p[0] += d->v[0] * dt, d->p[1] += d->v[1] * dt;
            vl = sqrtf(d->v[0] * d->v[0] + d->v[1] * d->v[1]);
            if (Fl < hold * 0.5f && vl < 0.01f) d->moving = false, d->v[0] = d->v[1] = 0.0f;
            // the trail: a point every so far, a tiny bead left behind now and then; it loses a little water
            float tx = d->ntrail ? d->trail[0][0] : d->p[0], ty = d->ntrail ? d->trail[0][1] : d->p[1];
            float moved = sqrtf((d->p[0] - tx) * (d->p[0] - tx) + (d->p[1] - ty) * (d->p[1] - ty));
            if (!d->ntrail || moved > 0.02f) {
                memmove(d->trail[1], d->trail[0], sizeof d->trail[0] * (LENS_TRAIL - 1));
                d->trail[0][0] = d->p[0], d->trail[0][1] = d->p[1], d->trail[0][2] = 0.0f;
                if (d->ntrail < LENS_TRAIL) d->ntrail++;
                if (L.trail_life > 0.0f) {
                    d->r *= 0.985f;
                    if (rnd() < 0.2f && L.n < LENS_MAX) {
                        float bx = d->p[0] - d->v[0] / (vl + 1e-4f) * d->r * 2.5f, by = d->p[1] - d->v[1] / (vl + 1e-4f) * d->r * 2.5f;
                        float br = d->r * 0.3f;
                        drop_add(bx, by, br);
                        d = &L.d[i]; // (the array didn't move: drop_add appends or reuses another slot)
                        L.d[L.n - 1].alpha = 1.0f;
                    }
                }
            }
        }
        d->r -= dry * dt;
        if (d->r < 0.0015f || d->p[0] < -d->r * 3.0f || d->p[0] > asp + d->r * 3.0f || d->p[1] < -d->r * 3.0f || d->p[1] > 1.0f + d->r * 3.0f) {
            drop_remove(i);
            continue;
        }
        i++;
    }
    // merging: two drops touching become one (its volume; the faster one carries it)
    for (uint32_t i = 0; i < L.n; i++)
        for (uint32_t j = i + 1; j < L.n; j++) {
            LensDrop *a = &L.d[i], *b = &L.d[j];
            float dx = a->p[0] - b->p[0], dy = a->p[1] - b->p[1], rr = (a->r + b->r) * 0.8f;
            if (dx * dx + dy * dy >= rr * rr) continue;
            float va = a->r * a->r * a->r, vb = b->r * b->r * b->r, s = 1.0f / (va + vb);
            if (b->r > a->r || (b->moving && !a->moving)) { LensDrop t = *a; *a = *b; *b = t; } // (a: the one that stays)
            a->p[0] = (a->p[0] * va + b->p[0] * vb) * s, a->p[1] = (a->p[1] * va + b->p[1] * vb) * s;
            a->v[0] = (a->v[0] * va + b->v[0] * vb) * s, a->v[1] = (a->v[1] * va + b->v[1] * vb) * s;
            a->r = cbrtf(va + vb), a->moving = a->moving || b->moving, a->alpha = fmaxf(a->alpha, b->alpha);
            a->flake = (a->flake * va + b->flake * vb) * s;
            drop_remove(j);
            j = i; // (again from the start: the bigger drop may touch others)
        }
}

// The tiles each item touches
static void bin(float* T, float x0, float y0, float x1, float y1, float item) {
    float sx = LENS_TX / L.aspect, sy = (float)LENS_TY;
    int a = (int)floorf(x0 * sx), b = (int)floorf(x1 * sx), c = (int)floorf(y0 * sy), d = (int)floorf(y1 * sy);
    if (a < 0) a = 0;
    if (c < 0) c = 0;
    if (b > LENS_TX - 1) b = LENS_TX - 1;
    if (d > LENS_TY - 1) d = LENS_TY - 1;
    for (int ty = c; ty <= d; ty++)
        for (int tx = a; tx <= b; tx++) {
            float* t = T + (size_t)(ty * LENS_TX + tx) * LENS_TILE_VEC4 * 4;
            int n = (int)t[0];
            if (n < LENS_TILE_ITEMS) t[1 + n] = item, t[0] = (float)(n + 1);
        }
}

bool lens_pack(float* dst, size_t floats) {
    if (floats < (size_t)LENS_VEC4 * 4) return false;
    float* T = dst + LENS_TILE_BASE * 4;
    memset(T, 0, sizeof(float) * LENS_TX * LENS_TY * LENS_TILE_VEC4 * 4);
    uint32_t ns = 0;
    for (uint32_t i = 0; i < L.n; i++) {
        const LensDrop* d = &L.d[i];
        float* o = dst + (LENS_DROP_BASE + 2 * i) * 4;
        float vl = sqrtf(d->v[0] * d->v[0] + d->v[1] * d->v[1]);
        float dx = vl > 1e-4f ? d->v[0] / vl : 0.0f, dy = vl > 1e-4f ? d->v[1] / vl : 1.0f;
        float st = 1.0f + clampf(vl * 6.0f, 0.0f, 1.5f); // (running: a tail behind it)
        o[0] = d->p[0], o[1] = d->p[1], o[2] = d->r, o[3] = d->alpha;
        o[4] = dx, o[5] = dy, o[6] = st, o[7] = d->flake;
        float e = d->r * st;
        bin(T, d->p[0] - e, d->p[1] - e, d->p[0] + e, d->p[1] + e, (float)i);
        if (L.trail_life <= 0.0f) continue;
        // its trail: from the drop back through the points it left
        float px = d->p[0], py = d->p[1];
        for (uint32_t k = 0; k < d->ntrail && ns < LENS_MAX * (LENS_TRAIL - 1); k++) {
            float qx = d->trail[k][0], qy = d->trail[k][1];
            if (k == 0 && fabsf(qx - px) + fabsf(qy - py) < 1e-4f) continue;
            float* s = dst + (LENS_SEG_BASE + 2 * ns) * 4;
            float w = d->r * 0.35f, a = 1.0f - d->trail[k][2] / L.trail_life;
            s[0] = px, s[1] = py, s[2] = qx, s[3] = qy, s[4] = w, s[5] = a > 0.0f ? a : 0.0f, s[6] = s[7] = 0.0f;
            bin(T, fminf(px, qx) - w, fminf(py, qy) - w, fmaxf(px, qx) + w, fmaxf(py, qy) + w, (float)(LENS_MAX + ns));
            ns++;
            px = qx, py = qy;
        }
    }
    dst[0] = (float)L.n, dst[1] = (float)ns, dst[2] = LENS_TX, dst[3] = LENS_TY;
    return true;
}

// --- The self-test: the simulation alone, a camera held and moved by hand ---
static void cam_look(LensCamera* c, float yaw_deg, float pitch_deg) {
    float y = yaw_deg * 0.017453293f, p = pitch_deg * 0.017453293f;
    c->fwd[0] = cosf(p) * cosf(y), c->fwd[1] = cosf(p) * sinf(y), c->fwd[2] = sinf(p);
    c->right[0] = sinf(y), c->right[1] = -cosf(y), c->right[2] = 0.0f;
    c->up[0] = -sinf(p) * cosf(y), c->up[1] = -sinf(p) * sinf(y), c->up[2] = cosf(p);
}
// the running drops' mean velocity along an axis (0 x, 1 y), and how many run
static float run_v(int axis, uint32_t* n) {
    float sum = 0.0f;
    *n = 0;
    for (uint32_t i = 0; i < L.n; i++)
        if (L.d[i].moving) sum += L.d[i].v[axis], (*n)++;
    return *n ? sum / (float)*n : 0.0f;
}
int lens_selftest(void) {
    int fails = 0;
    LensParams p = { 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 8.0f, 1.0f };
    LensCamera c = { .aspect = 16.0f / 9.0f };
    uint32_t run_level, run_floor, run_roll, run_push;
    // 1. looking level in the rain: drops gather and the big ones run down the view
    lens_reset(), L.rng = 12345u;
    cam_look(&c, 0.0f, 0.0f);
    for (int f = 0; f < 120; f++) lens_step(&p, &c, 60.0f, false, false, 1.0f / 60.0f);
    uint32_t n_level = L.n;
    float vy_level = run_v(1, &run_level), vx_level = run_v(0, &run_level);
    // 2. looking straight down: they hold where they are
    lens_reset(), L.rng = 12345u;
    cam_look(&c, 0.0f, -89.0f);
    for (int f = 0; f < 120; f++) lens_step(&p, &c, 60.0f, false, false, 1.0f / 60.0f);
    run_v(1, &run_floor);
    // 3. rolled 60 degrees: they run aslant, toward the screen's left
    lens_reset(), L.rng = 12345u;
    cam_look(&c, 0.0f, 0.0f);
    {
        float a = 1.0472f, r[3], u[3];
        for (int k = 0; k < 3; k++) r[k] = c.right[k] * cosf(a) + c.up[k] * sinf(a), u[k] = -c.right[k] * sinf(a) + c.up[k] * cosf(a);
        memcpy(c.right, r, sizeof r), memcpy(c.up, u, sizeof u);
    }
    for (int f = 0; f < 120; f++) lens_step(&p, &c, 60.0f, false, false, 1.0f / 60.0f);
    float vx_roll = run_v(0, &run_roll);
    // 4. a quick whip of the camera throws most of them off; 5. out of the rain they dry
    lens_reset(), L.rng = 12345u;
    cam_look(&c, 0.0f, 0.0f);
    for (int f = 0; f < 120; f++) lens_step(&p, &c, 60.0f, false, false, 1.0f / 60.0f);
    uint32_t before = L.n;
    for (int f = 0; f < 20; f++) cam_look(&c, (f & 1) ? 25.0f : -25.0f, 0.0f), lens_step(&p, &c, 0.0f, false, false, 1.0f / 60.0f);
    uint32_t after_whip = L.n;
    lens_reset(), L.rng = 12345u;
    cam_look(&c, 0.0f, 0.0f);
    for (int f = 0; f < 120; f++) lens_step(&p, &c, 60.0f, false, false, 1.0f / 60.0f);
    for (int f = 0; f < 60 * 10; f++) lens_step(&p, &c, 0.0f, true, false, 1.0f / 60.0f);
    uint32_t after_dry = L.n;
    // 6. the inertia, no gravity: the camera speeding up to the right (~2 g), the drops slide left
    lens_reset(), L.rng = 12345u;
    p.shake_off = 0.0f, p.gravity = 0.0f;
    cam_look(&c, 0.0f, 0.0f);
    for (int f = 0; f < 120; f++) lens_step(&p, &c, 60.0f, false, false, 1.0f / 60.0f);
    for (int f = 0; f < 12; f++) {
        for (int k = 0; k < 3; k++) c.pos[k] += c.right[k] * (float)f * 0.6f;
        lens_step(&p, &c, 0.0f, false, false, 1.0f / 60.0f);
    }
    float vx_push = run_v(0, &run_push);
    // 7. snow: flakes stick (none runs) and melt into drops
    lens_reset(), L.rng = 12345u;
    p.shake_off = 1.0f, p.gravity = 1.0f;
    c.pos[0] = c.pos[1] = c.pos[2] = 0.0f;
    cam_look(&c, 0.0f, 0.0f);
    for (int f = 0; f < 60; f++) lens_step(&p, &c, 60.0f, false, true, 1.0f / 60.0f);
    uint32_t run_flakes;
    run_v(1, &run_flakes);
    float flake0 = L.n ? L.d[0].flake : 0.0f;
    for (int f = 0; f < 60 * 6; f++) lens_step(&p, &c, 0.0f, false, true, 1.0f / 60.0f);
    float flake1 = 1.0f;
    for (uint32_t i = 0; i < L.n; i++) flake1 = fminf(flake1, L.d[i].flake);
    bool snow_ok = L.n > 0 && run_flakes == 0 && flake0 > 0.5f && flake1 < 0.05f;
    printf("[RealEngine] lens snow: %u flakes, %u running, melted to %.2f: %s\n", L.n, run_flakes, flake1, snow_ok ? "ok" : "wrong");
    bool ok = snow_ok && n_level > 20 && run_level > 0 && vy_level > 0.02f && fabsf(vx_level) < vy_level && run_floor == 0 && run_roll > 0 &&
              vx_roll < -0.02f && after_whip < before / 2 && after_dry == 0 && run_push > 0 && vx_push < -0.02f;
    printf("[RealEngine] lens water: %u drops; level %u run down at %.3f; looking down %u run; rolled they run sideways at %.3f; "
           "a whip left %u of %u; dried to %u; pushed right %u slide at %.3f: %s\n",
           n_level, run_level, vy_level, run_floor, vx_roll, after_whip, before, after_dry, run_push, vx_push, ok ? "ok" : "wrong");
    if (!ok) printf("[RealEngine] FAIL: lens water\n"), fails++;
    lens_reset();
    return fails;
}
