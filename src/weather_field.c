// (Q27.6) The regional weather: storm cells summed (weather_field.h)
#include "weather_field.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

typedef struct WStorm {
    float x, y, radius, peak, age, life, vx, vy; // cm, cm, mm/h, s, cm/s
    bool on, authored;
} WStorm;

static struct {
    bool on;
    float storminess, wind_speed, wind_dir;
    uint32_t seed, rng;
    WStorm s[WFIELD_MAX_STORMS];
    float spawn_t;
    bool fresh; // (a new field: its first storms anywhere around, part way through their lives)
} F;

static float rnd(void) { // (xorshift: the same storms for the same seed)
    uint32_t x = F.rng ? F.rng : 0x9E3779B9u;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    F.rng = x;
    return (float)(x >> 8) / 16777216.0f;
}
static float clampf(float v, float a, float b) { return v < a ? a : v > b ? b : v; }
static float smooth(float a, float b, float x) {
    float t = clampf((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
// grows in its first 15%, holds, fades over its last 30%
static float envelope(const WStorm* s) { return smooth(0.0f, 0.15f * s->life, s->age) * (1.0f - smooth(0.7f * s->life, s->life, s->age)); }

void wfield_configure(bool on, float storminess, uint32_t seed, float wind_speed, float wind_dir) {
    if (on && (!F.on || seed != F.seed)) { // (a new field: the seed's storms, already part way through their lives)
        memset(F.s, 0, sizeof F.s);
        F.seed = seed, F.rng = seed * 2654435761u + 1u, F.spawn_t = 0.0f, F.fresh = true;
    }
    F.on = on, F.storminess = clampf(storminess, 0.0f, 1.0f), F.wind_speed = wind_speed, F.wind_dir = wind_dir;
}
bool wfield_on(void) { return F.on; }

static void steer(float* vx, float* vy) { // (storms drift with the wind aloft: faster than the surface's, the same way)
    float a = F.wind_dir * 0.017453293f, sp = (F.wind_speed * 1.5f + 3.0f) * 100.0f;
    *vx = cosf(a) * sp, *vy = sinf(a) * sp;
}

int wfield_add_storm(float x, float y, float radius, float peak, float seconds) {
    for (int i = 0; i < WFIELD_MAX_STORMS; i++)
        if (!F.s[i].on) {
            WStorm* s = &F.s[i];
            memset(s, 0, sizeof *s);
            s->on = s->authored = true, s->x = x, s->y = y, s->radius = fmaxf(radius, 1000.0f), s->peak = fmaxf(peak, 0.0f);
            s->life = fmaxf(seconds, 10.0f), s->age = 0.15f * s->life; // (an authored one: at its full rate now)
            steer(&s->vx, &s->vy);
            return i;
        }
    return -1;
}
void wfield_clear(void) { memset(F.s, 0, sizeof F.s); }

void wfield_update(float dt, float cam_x, float cam_y) {
    if (!F.on) return;
    uint32_t alive = 0, wild = 0;
    for (int i = 0; i < WFIELD_MAX_STORMS; i++) {
        WStorm* s = &F.s[i];
        if (!s->on) continue;
        s->age += dt, s->x += s->vx * dt, s->y += s->vy * dt;
        float dx = s->x - cam_x, dy = s->y - cam_y;
        if (s->age >= s->life || (!s->authored && dx * dx + dy * dy > 1.5e6f * 1.5e6f)) s->on = false; // (over, or 15 km away)
        else alive++, wild += !s->authored;
    }
    (void)alive;
    // the rate's count of storms (0..4), born 2-6 km upwind and drifting over; at most one every 20 s
    uint32_t want = (uint32_t)floorf(F.storminess * 4.0f + 0.5f);
    F.spawn_t -= dt;
    if (wild < want && F.spawn_t <= 0.0f) {
        for (int i = 0; i < WFIELD_MAX_STORMS; i++)
            if (!F.s[i].on) {
                WStorm* s = &F.s[i];
                memset(s, 0, sizeof *s);
                float a = F.wind_dir * 0.017453293f, ux = cosf(a), uy = sinf(a);
                bool first = F.fresh;
                float back = (first ? -3.0f + 9.0f * rnd() : 2.0f + 4.0f * rnd()) * 1e5f, side = (rnd() * 2.0f - 1.0f) * 4e5f;
                s->on = true, s->x = cam_x - ux * back - uy * side, s->y = cam_y - uy * back + ux * side;
                s->radius = (400.0f + 1400.0f * rnd()) * 100.0f, s->peak = 8.0f + (17.0f + 75.0f * F.storminess) * rnd();
                s->life = 600.0f + 1200.0f * rnd(), s->age = first ? s->life * 0.5f * rnd() : 0.0f;
                steer(&s->vx, &s->vy);
                break;
            }
        wild++;
        F.spawn_t = F.fresh && wild < want ? 0.0f : 20.0f;
    }
    if (wild >= want) F.fresh = false;
}

void wfield_sample(float x, float y, WeatherSample* o) {
    float a = F.wind_dir * 0.017453293f;
    o->rain = 0.0f, o->clouds = 0.0f, o->gust = 0.0f, o->wind[0] = cosf(a) * F.wind_speed, o->wind[1] = sinf(a) * F.wind_speed;
    if (!F.on) return;
    for (int i = 0; i < WFIELD_MAX_STORMS; i++) {
        const WStorm* s = &F.s[i];
        if (!s->on) continue;
        float e = envelope(s), dx = x - s->x, dy = y - s->y, r2 = (dx * dx + dy * dy) / (s->radius * s->radius);
        o->rain += s->peak * e * expf(-r2);
        o->clouds += e * fminf(s->peak / 20.0f, 1.0f) * expf(-r2 / 3.2f); // (the anvil: wider than the rain)
        // the gust front: out from the core, strongest near its edge (~6 m/s for a strong one)
        float d = sqrtf(r2), g = e * fminf(s->peak / 40.0f, 1.5f) * 6.0f * d * expf(0.5f - 0.5f * r2), l = sqrtf(dx * dx + dy * dy);
        if (l > 1.0f) o->wind[0] += g * dx / l, o->wind[1] += g * dy / l;
        o->gust += g;
    }
    o->clouds = clampf(o->clouds, 0.0f, 1.0f);
    if (o->rain < 0.05f) o->rain = 0.0f;
}

uint32_t wfield_pack(float out[WFIELD_MAX_STORMS][4]) {
    uint32_t n = 0;
    for (int i = 0; F.on && i < WFIELD_MAX_STORMS; i++) {
        const WStorm* s = &F.s[i];
        if (!s->on) continue;
        out[n][0] = s->x, out[n][1] = s->y, out[n][2] = s->radius, out[n][3] = s->peak * envelope(s);
        n++;
    }
    return n;
}

int wfield_selftest(void) {
    int fails = 0;
    wfield_configure(true, 0.0f, 7u, 5.0f, 0.0f);
    wfield_clear();
    wfield_add_storm(150000.0f, 0.0f, 50000.0f, 60.0f, 3600.0f);
    WeatherSample a, b;
    wfield_sample(0.0f, 0.0f, &a), wfield_sample(150000.0f, 0.0f, &b);
    printf("[RealEngine] weather field: here %.2f mm/h, clouds %.2f; the storm's core 1.5 km away %.1f mm/h, clouds %.2f\n", a.rain, a.clouds, b.rain, b.clouds);
    if (a.rain > 0.05f || b.rain < 50.0f || b.clouds < 0.9f) printf("[RealEngine] FAIL: weather field: sun here, the storm there\n"), fails++;
    // it drifts with the wind (+x at 5 m/s: aloft 10.5 m/s): 60 s later 630 m further
    wfield_update(60.0f, 0.0f, 0.0f);
    float st[WFIELD_MAX_STORMS][4];
    uint32_t n = wfield_pack(st);
    if (n != 1 || fabsf(st[0][0] - (150000.0f + 63000.0f)) > 100.0f) printf("[RealEngine] FAIL: weather field: the storm didn't drift (%u, %.0f)\n", n, n ? st[0][0] : 0.0f), fails++;
    // a stormy field keeps its count, born upwind
    wfield_clear();
    wfield_configure(true, 1.0f, 7u, 5.0f, 0.0f);
    for (int k = 0; k < 200; k++) wfield_update(1.0f, 0.0f, 0.0f);
    n = wfield_pack(st);
    printf("[RealEngine] weather field: stormy, after 200 s: %u storms\n", n);
    if (n < 3) printf("[RealEngine] FAIL: weather field: a stormy field should keep ~4 storms\n"), fails++;
    wfield_configure(false, 0.0f, 0u, 0.0f, 0.0f);
    wfield_clear();
    return fails;
}
