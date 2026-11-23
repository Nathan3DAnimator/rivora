// (Q27.10) The city's routines: the clock, the Smart Objects, the world's events (city.h)
#include "city.h"

#include <math.h>
#include <string.h>

#include "components.h"
#include "crowd.h"
#include "platform.h"

#define CITY_MAX_SO 256
#define CITY_EVENTS 64
#define CITY_CALLS 16

static struct {
    float hour, scale;
    bool clock_set;
    CitySO so[CITY_MAX_SO];
    uint32_t nso;
    CityEvent ev[CITY_EVENTS];
    uint32_t nev, ev_head; // (a ring: ev_head the oldest)
    CityEvent ordered[CITY_EVENTS];
    struct { float pos[3]; double at; bool on; } call[CITY_CALLS];
    double now;
    uint32_t rng;
    CityStats st;
} K = { 9.0f, 60.0f };

static float rnd(void) {
    uint32_t x = K.rng ? K.rng : 0x6C8E9CF5u;
    x ^= x << 13, x ^= x >> 17, x ^= x << 5;
    K.rng = x;
    return (float)(x >> 8) / 16777216.0f;
}

static void post(uint32_t type, const float at[3]) {
    CityEvent* e;
    if (K.nev < CITY_EVENTS) e = &K.ev[(K.ev_head + K.nev++) % CITY_EVENTS];
    else e = &K.ev[K.ev_head], K.ev_head = (K.ev_head + 1u) % CITY_EVENTS;
    e->type = type, e->hour = K.hour;
    memcpy(e->pos, at, sizeof e->pos);
    K.st.events++;
}

void city_reset(void) {
    K.nev = K.ev_head = 0;
    memset(K.call, 0, sizeof K.call);
    K.st.police_calls = K.st.events = 0;
}

float city_hour(void) { return K.hour; }
float city_time_scale(void) { return K.scale; }
void city_set_time(float hour, float scale) {
    K.hour = fmodf(fmaxf(hour, 0.0f), 24.0f);
    if (scale > 0.0f) K.scale = scale;
    K.clock_set = true;
}

// open within [open, close) (wrapping past midnight: 22 to 6); both 0: always
static bool open_at(float h, float open, float close) {
    if (open == close) return true;
    return open < close ? h >= open && h < close : h >= open || h < close;
}

void city_update(World* w, float dt, bool playing) {
    double t0 = re_time_seconds();
    if (playing) {
        K.now += dt;
        K.hour = fmodf(K.hour + dt * K.scale / 3600.0f, 24.0f);
    }
    // the Smart Objects: the Interactables with slots (their visitors kept by entity across frames)
    uint32_t keep_n = K.nso;
    static CitySO prev[CITY_MAX_SO];
    memcpy(prev, K.so, sizeof(CitySO) * keep_n);
    K.nso = 0;
    uint32_t open = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_INTERACTABLE)) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const CInteractable* ia = ecs_arch_column(a, COMP_INTERACTABLE);
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && K.nso < CITY_MAX_SO; i++) {
            if (!ia[i].so_slots) continue;
            CitySO* s = &K.so[K.nso++];
            s->e = ents[i], s->slots = ia[i].so_slots;
            memcpy(s->pos, tr[i].location, sizeof s->pos);
            s->open = open_at(K.hour, (float)ia[i].open_q * 0.25f, (float)ia[i].close_q * 0.25f);
            s->visitors = 0;
            for (uint32_t k = 0; k < keep_n; k++)
                if (prev[k].e == s->e) s->visitors = prev[k].visitors;
            open += s->open;
        }
    }
    // the witnesses' calls coming through
    for (int k = 0; k < CITY_CALLS; k++)
        if (K.call[k].on && K.now >= K.call[k].at) {
            K.call[k].on = false;
            post(CITY_EV_POLICE_CALLED, K.call[k].pos);
            K.st.police_calls++;
        }
    uint32_t vis = 0;
    for (uint32_t k = 0; k < K.nso; k++) vis += K.so[k].visitors;
    K.st.objects = K.nso, K.st.open = open, K.st.visitors = vis;
    K.st.ms = (re_time_seconds() - t0) * 1000.0;
}

uint32_t city_smart_objects(CitySO** out) {
    *out = K.so;
    return K.nso;
}
bool city_is_open(Entity e) {
    for (uint32_t k = 0; k < K.nso; k++)
        if (K.so[k].e == e) return K.so[k].open;
    return false;
}
uint32_t city_visitors(Entity e) {
    for (uint32_t k = 0; k < K.nso; k++)
        if (K.so[k].e == e) return K.so[k].visitors;
    return 0;
}

void city_report_crime(const float at[3], float radius) {
    post(CITY_EV_CRIME, at);
    crowd_alarm(at, radius);
    float wp[3];
    if (crowd_nearest(at, radius, wp)) // the nearest who saw it calls (a phone's 2-4 s)
        for (int k = 0; k < CITY_CALLS; k++)
            if (!K.call[k].on) {
                K.call[k].on = true, K.call[k].at = K.now + 2.0 + 2.0 * rnd();
                memcpy(K.call[k].pos, wp, sizeof wp);
                break;
            }
}
uint32_t city_police_calls(void) { return K.st.police_calls; }
uint32_t city_events(const CityEvent** out) {
    for (uint32_t i = 0; i < K.nev; i++) K.ordered[i] = K.ev[(K.ev_head + i) % CITY_EVENTS];
    *out = K.ordered;
    return K.nev;
}
const CityStats* city_stats(void) { return &K.st; }
