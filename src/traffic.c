// Traffic (Q27.2): traffic.h, notas-dev/transito.md
#include "traffic.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "platform.h"
#include "render.h"
#include "spline.h"

#define TR_MAX_ROADS 1024
#define TR_MAX_LANES 4096
#define TR_MAX_NEXT 4
#define TR_MAX_CARS 16384
#define TR_JOIN 600.0f       // cm: a road's end and another's start this near join
#define TR_CAR_LEN 450.0f    // cm
#define TR_STOP_BACK 300.0f  // cm before a lane's end: its stop line
#define TR_FAR 15000.0f      // cm: past it a car steps every other frame

typedef struct Lane {
    uint16_t road;
    int8_t dir;              // +1 along the curve's points, -1 against them
    float offset;            // cm to the right of its way
    float length, v0;        // cm, cm/s
    uint16_t next[TR_MAX_NEXT];
    uint8_t nnext;
    int16_t node;            // the junction at its end (-1: none)
    uint8_t phase;           // its approach's signal group there
    float end[3], start[3], end_dir[2];
    // the cars on it, nearest its start first (rebuilt every frame)
    uint32_t first, count;
} Lane;

typedef struct Node { float x, y; uint8_t nin; bool signal; float green; } Node;

static struct {
    bool playing;
    SplineCurve* curve;      // [TR_MAX_ROADS]
    uint32_t nroads, nlanes, nnodes;
    Lane lane[TR_MAX_LANES];
    Node node[TR_MAX_LANES];
    // the cars (structure of arrays)
    uint32_t n;
    uint16_t *ln, *nxt;
    float *s, *v, *vk, *a, *b, *T, *s0;
    Entity* e;
    uint32_t *order, *tmp;   // the cars by lane and distance
    float time;
    uint32_t frame;
    TrafficStats stats;
    World* w;
    float cam[3];
} T;

static uint32_t hash32(uint32_t x) {
    x ^= x >> 16, x *= 0x7FEB352Du, x ^= x >> 15, x *= 0x846CA68Bu, x ^= x >> 16;
    return x;
}
static float rand01(uint32_t x) { return (float)(hash32(x) >> 8) / 16777216.0f; }

// A lane's point at s (cm from its start along its way) and its heading
static void lane_at(const Lane* L, float s, float pos[3], float fwd[3]) {
    const SplineCurve* c = &T.curve[L->road];
    float d = L->dir > 0 ? s : c->length - s;
    d = fminf(fmaxf(d, 0.0f), c->length);
    float dir[3];
    spline_location_at_distance(c, d, pos);
    spline_direction_at_distance(c, d, dir);
    if (L->dir < 0) dir[0] = -dir[0], dir[1] = -dir[1], dir[2] = -dir[2];
    float h = sqrtf(dir[0] * dir[0] + dir[1] * dir[1]);
    float rx = h > 1e-6f ? dir[1] / h : 0.0f, ry = h > 1e-6f ? -dir[0] / h : 0.0f; // (right of the way, Z up)
    pos[0] += rx * L->offset, pos[1] += ry * L->offset;
    if (fwd) memcpy(fwd, dir, sizeof dir);
}

// Is the lane's signal green now (no node or no signals: always)
static bool lane_green(const Lane* L) {
    if (L->node < 0) return true;
    const Node* N = &T.node[L->node];
    if (!N->signal) return true;
    float cycle = N->green * 2.0f, t = fmodf(T.time + (float)L->node * 3.7f, cycle), in = fmodf(t, N->green);
    return in > 2.0f && (uint32_t)(t / N->green) == L->phase; // (the first 2 s of each phase: all red)
}

static void build_network(World* w) {
    T.nroads = T.nlanes = T.nnodes = 0;
    if (!T.curve) T.curve = malloc(sizeof(SplineCurve) * TR_MAX_ROADS);
    if (!T.curve) return;
    const uint64_t need = COMP_BIT(COMP_SPLINE) | COMP_BIT(COMP_TRANSFORM);
    float green[TR_MAX_ROADS];
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if ((a->mask & need) != need) continue;
        const Entity* ents = ecs_arch_entities(a);
        const CRail* rl = ecs_arch_column(a, COMP_SPLINE);
        for (uint32_t i = 0; i < a->count && T.nroads < TR_MAX_ROADS; i++) {
            if (!rl[i].road_lanes || rl[i].npoints < 2) continue;
            uint32_t ri = T.nroads;
            if (!spline_world_curve(w, ents[i], &T.curve[ri]) || T.curve[ri].length < 100.0f) continue;
            T.nroads++;
            green[ri] = rl[i].road_signal;
            uint32_t lanes = rl[i].road_lanes > 4 ? 4 : rl[i].road_lanes;
            float wdt = rl[i].road_lane_width > 50.0f ? rl[i].road_lane_width : 350.0f;
            float v0 = (rl[i].road_speed > 1.0f ? rl[i].road_speed : 50.0f) / 3.6f * 100.0f;
            for (int dir = 1; dir >= -1; dir -= 2) {
                if (dir < 0 && rl[i].road_oneway) break;
                for (uint32_t k = 0; k < lanes && T.nlanes < TR_MAX_LANES; k++) {
                    Lane* L = &T.lane[T.nlanes++];
                    memset(L, 0, sizeof *L);
                    L->road = (uint16_t)ri, L->dir = (int8_t)dir, L->length = T.curve[ri].length, L->v0 = v0, L->node = -1;
                    L->offset = rl[i].road_oneway ? ((float)k - (float)(lanes - 1) * 0.5f) * wdt : ((float)k + 0.5f) * wdt;
                    float f[3];
                    lane_at(L, 0.0f, L->start, NULL);
                    lane_at(L, L->length, L->end, f);
                    float h = sqrtf(f[0] * f[0] + f[1] * f[1]);
                    L->end_dir[0] = h > 1e-6f ? f[0] / h : 1.0f, L->end_dir[1] = h > 1e-6f ? f[1] / h : 0.0f;
                }
            }
        }
    }
    // the junctions: a lane's end and the starts within TR_JOIN of it (not its own road back); its node: the ends near it
    for (uint32_t i = 0; i < T.nlanes; i++) {
        Lane* L = &T.lane[i];
        for (uint32_t j = 0; j < T.nlanes && L->nnext < TR_MAX_NEXT; j++) {
            const Lane* M = &T.lane[j];
            if (M->road == L->road) continue;
            float dx = M->start[0] - L->end[0], dy = M->start[1] - L->end[1];
            if (dx * dx + dy * dy < TR_JOIN * TR_JOIN * 4.0f) L->next[L->nnext++] = (uint16_t)j;
        }
        if (!L->nnext) continue;
        int nd = -1;
        for (uint32_t k = 0; k < T.nnodes && nd < 0; k++) {
            float dx = T.node[k].x - L->end[0], dy = T.node[k].y - L->end[1];
            if (dx * dx + dy * dy < TR_JOIN * TR_JOIN * 4.0f) nd = (int)k;
        }
        if (nd < 0 && T.nnodes < TR_MAX_LANES) {
            nd = (int)T.nnodes++;
            memset(&T.node[nd], 0, sizeof T.node[nd]);
            T.node[nd].x = L->end[0], T.node[nd].y = L->end[1];
        }
        if (nd < 0) continue;
        L->node = (int16_t)nd;
        T.node[nd].nin++;
        if (green[L->road] > 0.0f) T.node[nd].green = fmaxf(T.node[nd].green, green[L->road]);
        // its approach's group: the axis it comes along (two phases: one axis, then the other)
        L->phase = fabsf(L->end_dir[0]) >= fabsf(L->end_dir[1]) ? 0 : 1;
    }
    T.stats.signals = 0;
    for (uint32_t k = 0; k < T.nnodes; k++) {
        T.node[k].signal = T.node[k].nin >= 3 && T.node[k].green > 2.5f;
        T.stats.signals += T.node[k].signal;
    }
    T.stats.roads = T.nroads, T.stats.lanes = T.nlanes, T.stats.junctions = T.nnodes;
}

static void cars_free(void) {
    free(T.ln), free(T.nxt), free(T.s), free(T.v), free(T.vk), free(T.a), free(T.b), free(T.T), free(T.s0), free(T.e), free(T.order), free(T.tmp);
    T.ln = T.nxt = NULL, T.s = T.v = T.vk = T.a = T.b = T.T = T.s0 = NULL, T.e = NULL, T.order = T.tmp = NULL, T.n = 0;
}

static uint16_t pick_next(const Lane* L, uint32_t car, uint32_t salt) {
    if (!L->nnext) return 0xFFFFu;
    return L->next[hash32(car * 7919u + salt) % L->nnext];
}

void traffic_begin_play(World* w) {
    T.playing = true, T.time = 0.0f, T.frame = 0, T.w = w;
    memset(&T.stats, 0, sizeof T.stats);
    build_network(w);
    cars_free();
    // the cars: density per 100 m of lane, spread along each lane at even gaps (a seed's jitter)
    uint32_t want = 0;
    float dens[TR_MAX_LANES];
    {
        const uint64_t need = COMP_BIT(COMP_SPLINE) | COMP_BIT(COMP_TRANSFORM);
        uint32_t ri = 0;
        float road_dens[TR_MAX_ROADS];
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if ((a->mask & need) != need) continue;
            const CRail* rl = ecs_arch_column(a, COMP_SPLINE);
            for (uint32_t i = 0; i < a->count && ri < T.nroads; i++)
                if (rl[i].road_lanes && rl[i].npoints >= 2) road_dens[ri++] = rl[i].road_density > 0.0f ? rl[i].road_density : 4.0f;
        }
        for (uint32_t l = 0; l < T.nlanes; l++) {
            dens[l] = T.lane[l].road < ri ? road_dens[T.lane[l].road] : 4.0f;
            uint32_t k = (uint32_t)(T.lane[l].length / 10000.0f * dens[l]);
            uint32_t room = (uint32_t)(T.lane[l].length / (TR_CAR_LEN + 300.0f));
            want += k < room ? k : room;
        }
    }
    if (want > TR_MAX_CARS) want = TR_MAX_CARS;
    if (!want) return;
    T.ln = malloc(sizeof(uint16_t) * want), T.nxt = malloc(sizeof(uint16_t) * want);
    float** fs[] = { &T.s, &T.v, &T.vk, &T.a, &T.b, &T.T, &T.s0 };
    bool ok = T.ln && T.nxt;
    for (size_t i = 0; i < sizeof fs / sizeof fs[0]; i++) ok &= (*fs[i] = malloc(sizeof(float) * want)) != NULL;
    T.e = malloc(sizeof(Entity) * want), T.order = malloc(sizeof(uint32_t) * want), T.tmp = malloc(sizeof(uint32_t) * want);
    if (!ok || !T.e || !T.order || !T.tmp) {
        cars_free();
        return;
    }
    for (uint32_t l = 0; l < T.nlanes && T.n < want; l++) {
        uint32_t k = (uint32_t)(T.lane[l].length / 10000.0f * dens[l]);
        uint32_t room = (uint32_t)(T.lane[l].length / (TR_CAR_LEN + 300.0f));
        if (k > room) k = room;
        float step = T.lane[l].length / (float)(k ? k : 1);
        for (uint32_t q = 0; q < k && T.n < want; q++) {
            uint32_t c = T.n++;
            T.ln[c] = (uint16_t)l;
            T.s[c] = step * (float)q + step * 0.3f * rand01(c * 3u + 1u);
            // a driver: its desired speed (0.85-1.15 of the limit), acceleration 1.2-2.2 m/s2, comfortable braking
            // 2-3 m/s2, time headway 1.0-1.8 s, standstill gap 1.5-2.5 m
            T.vk[c] = 0.85f + 0.3f * rand01(c * 3u + 2u);
            T.a[c] = 120.0f + 100.0f * rand01(c * 7u + 3u), T.b[c] = 200.0f + 100.0f * rand01(c * 11u + 5u);
            T.T[c] = 1.0f + 0.8f * rand01(c * 13u + 7u), T.s0[c] = 150.0f + 100.0f * rand01(c * 17u + 11u);
            T.v[c] = T.lane[l].v0 * T.vk[c] * 0.5f;
            T.nxt[c] = pick_next(&T.lane[l], c, 0);
            // its actor: a box the size of a car, a color of its own
            Entity e = ecs_create(w, COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
            T.e[c] = e;
            CTransform* t = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
            CStaticMesh* m = e ? ecs_get(w, e, COMP_STATIC_MESH) : NULL;
            if (t) t->scale[0] = 4.4f, t->scale[1] = 1.8f, t->scale[2] = 1.4f, t->flags = MOBILITY_MOVABLE;
            if (m) {
                static const float pal[6][3] = { { 0.6f, 0.05f, 0.04f }, { 0.05f, 0.1f, 0.5f }, { 0.75f, 0.75f, 0.72f },
                                                 { 0.03f, 0.03f, 0.03f }, { 0.4f, 0.42f, 0.45f }, { 0.65f, 0.5f, 0.1f } };
                m->mesh = SHAPE_CUBE, m->metallic = 0.6f, m->roughness = 0.3f, m->cast_shadow = 1;
                memcpy(m->base_color, pal[hash32(c) % 6u], 12);
            }
        }
    }
    T.stats.cars = T.n;
}

void traffic_end_play(World* w) {
    (void)w;
    T.playing = false;
    cars_free(); // (the actors go with the play's world)
}

// The cars by lane, nearest the start first: a counting sort by lane, then an insertion sort by s within each (they were
// nearly in order: nobody passes in a lane)
static void sort_cars(void) {
    for (uint32_t l = 0; l < T.nlanes; l++) T.lane[l].count = 0;
    for (uint32_t c = 0; c < T.n; c++) T.lane[T.ln[c]].count++;
    uint32_t acc = 0;
    for (uint32_t l = 0; l < T.nlanes; l++) T.lane[l].first = acc, acc += T.lane[l].count, T.lane[l].count = 0;
    for (uint32_t c = 0; c < T.n; c++) {
        Lane* L = &T.lane[T.ln[c]];
        T.order[L->first + L->count++] = c;
    }
    for (uint32_t l = 0; l < T.nlanes; l++) {
        uint32_t* o = T.order + T.lane[l].first;
        for (uint32_t i = 1; i < T.lane[l].count; i++)
            for (uint32_t j = i; j > 0 && T.s[o[j]] < T.s[o[j - 1]]; j--) {
                uint32_t x = o[j];
                o[j] = o[j - 1], o[j - 1] = x;
            }
    }
}

typedef struct StepJob { float dt; uint32_t per; uint32_t red_runs[64]; } StepJob;

// The IDM's acceleration (cm/s2): the free road term and the car ahead (gap cm, its speed)
static float idm(uint32_t c, float v0, float gap, float vlead) {
    float v = T.v[c], a = T.a[c], b = T.b[c];
    float ss = T.s0[c] + fmaxf(0.0f, v * T.T[c] + v * (v - vlead) / (2.0f * sqrtf(a * b)));
    float g = fmaxf(gap, 1.0f), r = v / fmaxf(v0, 1.0f);
    return a * (1.0f - r * r * r * r - (ss / g) * (ss / g));
}

static void step_lanes(void* arg, int index) {
    StepJob* j = arg;
    uint32_t l0 = (uint32_t)index * j->per, l1 = l0 + j->per;
    if (l1 > T.nlanes) l1 = T.nlanes;
    for (uint32_t l = l0; l < l1; l++) {
        const Lane* L = &T.lane[l];
        const uint32_t* o = T.order + L->first;
        bool green = lane_green(L);
        for (uint32_t k = 0; k < L->count; k++) {
            uint32_t c = o[k];
            float dt = j->dt;
            // (far: every other frame, twice the time)
            float p[3];
            lane_at(L, T.s[c], p, NULL);
            float dx = p[0] - T.cam[0], dy = p[1] - T.cam[1];
            if (dx * dx + dy * dy > TR_FAR * TR_FAR) {
                if (((c ^ T.frame) & 1u) != 0u) continue;
                dt *= 2.0f;
            }
            float v0 = L->v0 * T.vk[c];
            // the car ahead: on this lane, else the last one on its next lane (the nearest its start)
            float gap = 1e9f, vlead = v0;
            if (k + 1 < L->count) {
                uint32_t q = o[k + 1];
                gap = T.s[q] - T.s[c] - TR_CAR_LEN, vlead = T.v[q];
            } else if (T.nxt[c] != 0xFFFFu) {
                const Lane* M = &T.lane[T.nxt[c]];
                if (M->count) {
                    uint32_t q = T.order[M->first];
                    gap = (L->length - T.s[c]) + T.s[q] - TR_CAR_LEN, vlead = T.v[q];
                }
            } else
                gap = L->length - T.s[c] - TR_CAR_LEN * 0.5f, vlead = 0.0f; // (a dead end: it stops at the end)
            // the signal: red, and room to stop before the line (b x 1.5): the line is a standing car
            float to_line = L->length - TR_STOP_BACK - T.s[c];
            if (!green && to_line > -50.0f) {
                float stop = T.v[c] * T.v[c] / (2.0f * T.b[c] * 1.5f);
                if (to_line > stop * 0.5f) {
                    if (to_line < gap) gap = to_line, vlead = 0.0f;
                }
            }
            float acc = idm(c, v0, gap, vlead);
            if (acc < -900.0f) acc = -900.0f;
            float v = fmaxf(T.v[c] + acc * dt, 0.0f);
            float ns = T.s[c] + v * dt;
            if (!green && T.s[c] <= L->length - TR_STOP_BACK && ns > L->length - TR_STOP_BACK && to_line > T.v[c] * T.v[c] / (2.0f * T.b[c] * 1.5f))
                j->red_runs[index & 63]++;
            T.v[c] = v, T.s[c] = ns;
        }
    }
}

// The cars onto their next lanes, their actors placed (in parallel: each car its own actor)
typedef struct PlaceJob { uint32_t per; } PlaceJob;
static void place_cars(void* arg, int index) {
    PlaceJob* j = arg;
    uint32_t c0 = (uint32_t)index * j->per, c1 = c0 + j->per;
    if (c1 > T.n) c1 = T.n;
    for (uint32_t c = c0; c < c1; c++) {
        CTransform* t = T.e[c] ? ecs_get(T.w, T.e[c], COMP_TRANSFORM) : NULL;
        if (!t) continue;
        const Lane* L = &T.lane[T.ln[c]];
        float p[3], f[3];
        lane_at(L, fminf(T.s[c], L->length), p, f);
        t->location[0] = p[0], t->location[1] = p[1], t->location[2] = p[2] + 70.0f;
        t->rotation[1] = atan2f(f[1], f[0]) * 57.29578f; // (the yaw: pitch, yaw, roll)
    }
}

void traffic_update(World* w, float dt, const float camera[3]) {
    if (!T.playing || !T.n) return;
    double t0 = re_time_seconds();
    T.w = w;
    if (dt > 0.1f) dt = 0.1f;
    T.time += dt, T.frame++;
    memcpy(T.cam, camera, sizeof T.cam);
    sort_cars();
    static StepJob job;
    memset(&job, 0, sizeof job);
    job.dt = dt, job.per = 64;
    re_parallel_for((int)((T.nlanes + job.per - 1) / job.per), step_lanes, &job);
    for (int k = 0; k < 64; k++) T.stats.red_runs += job.red_runs[k];
    // past their lane's end: onto the next (a dead end: back to its start, a car leaving the map comes back). A merge:
    // the next lane's start taken (by a car already on it, or one that came in this frame from another approach) -> it
    // waits at the line (two flows into one lane both saw the same car ahead and entered together: they overlapped)
    static float first_s[TR_MAX_LANES];
    for (uint32_t l = 0; l < T.nlanes; l++) first_s[l] = T.lane[l].count ? T.s[T.order[T.lane[l].first]] : 1e9f;
    for (uint32_t c = 0; c < T.n; c++) {
        const Lane* L = &T.lane[T.ln[c]];
        if (T.s[c] < L->length) continue;
        uint16_t nx = T.nxt[c];
        if (nx == 0xFFFFu) {
            T.s[c] = 0.0f, T.v[c] *= 0.5f;
            continue;
        }
        float ns = T.s[c] - L->length;
        if (first_s[nx] - ns < TR_CAR_LEN + 150.0f) {
            T.s[c] = L->length - 1.0f, T.v[c] = 0.0f;
            continue;
        }
        first_s[nx] = fminf(first_s[nx], ns);
        T.s[c] = ns, T.ln[c] = nx;
        T.nxt[c] = pick_next(&T.lane[nx], c, T.stats.passed + T.frame);
        T.stats.passed++;
    }
    PlaceJob pj = { 512 };
    re_parallel_for((int)((T.n + pj.per - 1) / pj.per), place_cars, &pj);
    // the stats: the smallest gap (the order of this frame's start), the stopped, the mean speed
    sort_cars();
    float mg = 1e9f, vs = 0.0f;
    uint32_t st = 0;
    for (uint32_t l = 0; l < T.nlanes; l++) {
        const uint32_t* o = T.order + T.lane[l].first;
        for (uint32_t k = 0; k + 1 < T.lane[l].count; k++) mg = fminf(mg, T.s[o[k + 1]] - T.s[o[k]] - TR_CAR_LEN);
    }
    for (uint32_t c = 0; c < T.n; c++) vs += T.v[c], st += T.v[c] < 28.0f;
    T.stats.min_gap = mg < 1e8f ? mg : 0.0f, T.stats.stopped = st, T.stats.mean_speed = vs / (float)T.n * 0.036f;
    T.stats.ms = (re_time_seconds() - t0) * 1000.0;
}

const TrafficStats* traffic_stats(void) { return &T.stats; }
