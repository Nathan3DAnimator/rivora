// Levels loaded in the background before they are opened (level_preload.h)
#include "level_preload.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "components.h"
#include "level.h"
#include "platform.h"
#include "platform_files.h"

#define SLICE (256u * 1024u) // the thread's read: a slice, then a pause the speed sets

typedef struct Preload {
    char name[128], path[700];
    volatile float speed;        // 0..1 (< 0: the global one)
    volatile int state;          // PreloadState
    volatile int cancel;
    volatile uint64_t done, total; // bytes read (the level and its lightmaps) of all
    World world;                 // built apart: no BeginPlay, not drawn
    LevelCamera cam;             // the file's camera
    bool world_made;
    ReThread* thread;
    uint32_t cursor, nbp, nbuilt; // (the main thread: the Blueprints' component trees, slot by slot)
    double read_ms, build_ms;
} Preload;

static struct {
    Preload p[PRELOAD_MAX];
    volatile float speed; // the global speed (every background load)
    PreloadStats st;
    bool init;
} P;

static void ensure_init(void) {
    if (!P.init) P.init = true, P.speed = 1.0f;
}
void level_async_set_speed(float s) {
    ensure_init();
    P.speed = s < 0.01f ? 0.01f : s > 1.0f ? 1.0f : s;
}
float level_async_speed(void) {
    ensure_init();
    return P.speed;
}
static float speed_of(const Preload* p) {
    float s = p->speed >= 0.0f ? p->speed : P.speed;
    return s < 0.01f ? 0.01f : s > 1.0f ? 1.0f : s;
}

static void base_name(const char* path, char* out, size_t cap) {
    const char* b = strrchr(path, '/');
    const char* b2 = strrchr(path, '\\');
    if (b2 > b) b = b2;
    snprintf(out, cap, "%s", b ? b + 1 : path);
    char* dot = strrchr(out, '.');
    if (dot && !_stricmp(dot, ".rlevel")) *dot = 0;
}
static Preload* find(const char* name) {
    if (!name || !name[0]) return NULL;
    char want[128];
    base_name(name, want, sizeof want);
    for (int i = 0; i < PRELOAD_MAX; i++)
        if (P.p[i].state != PRE_NONE && !_stricmp(P.p[i].name, want)) return &P.p[i];
    return NULL;
}

// a file read in slices, a pause after each as long as the speed asks (speed 1: none; 0.1: 9x the slice's time)
static uint8_t* paced_read(Preload* p, const char* path, size_t* size_out) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = n > 0 ? malloc((size_t)n) : NULL; // (a load on its thread, not a frame)
    size_t at = 0;
    while (d && at < (size_t)n && !p->cancel) {
        double t0 = re_time_seconds();
        size_t want = (size_t)n - at < SLICE ? (size_t)n - at : SLICE;
        if (fread(d + at, 1, want, f) != want) break;
        at += want, p->done += want;
        float s = speed_of(p);
        if (s < 0.999f) {
            double spent = re_time_seconds() - t0, pause = spent * (1.0 / s - 1.0) + (1.0 - s) * 0.004; // (+ a floor: a cached file reads in no time)
            re_sleep_seconds(fmin(pause, 0.06));
        }
    }
    fclose(f);
    if (!d || at != (size_t)n || p->cancel) return free(d), NULL;
    *size_out = (size_t)n;
    return d;
}

static int preload_thread(void* arg) {
    Preload* p = arg;
    re_thread_set_low_priority(true);
    double t0 = re_time_seconds();
    char lp[760];
    level_rlight_path(p->path, lp, sizeof lp);
    uint64_t sz_level = 0, sz_light = 0;
    re_file_stat(p->path, &sz_level, NULL, NULL);
    if (!re_file_stat(lp, &sz_light, NULL, NULL)) sz_light = 0;
    p->total = sz_level + sz_light + 1;
    size_t n = 0;
    uint8_t* d = paced_read(p, p->path, &n);
    bool ok = d && !p->cancel;
    if (ok) { // its world apart (the parse: a copy of rows, no asset touched)
        ok = ecs_init(&p->world);
        p->world_made = ok;
        ok = ok && level_load_memory(&p->world, d, n, &p->cam);
    }
    free(d);
    if (ok && sz_light && !p->cancel) { // its lightmaps through the OS cache: the switch reads them from memory
        size_t ln = 0;
        free(paced_read(p, lp, &ln));
    }
    p->done = p->total;
    p->read_ms = (re_time_seconds() - t0) * 1000.0;
    p->state = ok && !p->cancel ? PRE_BUILDING : PRE_FAILED;
    return 0;
}

static void slot_free(Preload* p) {
    if (p->thread) p->cancel = 1, re_thread_join(p->thread), p->thread = NULL;
    if (p->world_made) ecs_shutdown(&p->world);
    memset(p, 0, sizeof *p);
}

bool level_preload(const char* name, float speed) {
    ensure_init();
    Preload* p = find(name);
    if (p && p->state != PRE_FAILED) {
        p->speed = speed;
        return true;
    }
    if (p) slot_free(p);
    for (int i = 0; i < PRELOAD_MAX && !p; i++)
        if (P.p[i].state == PRE_NONE) p = &P.p[i];
    if (!p) {
        printf("[RealEngine] Preload Level: %d levels preloading already\n", PRELOAD_MAX);
        return false;
    }
    memset(p, 0, sizeof *p);
    if (!level_find(name, p->path, sizeof p->path)) {
        printf("[RealEngine] Preload Level: no level \"%s\" in the project\n", name ? name : "");
        return false;
    }
    base_name(p->path, p->name, sizeof p->name);
    p->speed = speed;
    p->state = PRE_READING;
    P.st.started++;
    p->thread = re_thread_start(preload_thread, p);
    if (!p->thread) preload_thread(p);
    return true;
}

void level_preload_set_speed(const char* name, float speed) {
    bool all = !name || !name[0];
    Preload* one = all ? NULL : find(name);
    for (int i = 0; i < PRELOAD_MAX; i++)
        if (P.p[i].state != PRE_NONE && (all || &P.p[i] == one)) P.p[i].speed = speed;
}

PreloadState level_preload_state(const char* name, float* progress) {
    Preload* p = find(name);
    if (progress) *progress = 0.0f;
    if (!p) return PRE_NONE;
    if (progress) {
        float read = p->total ? (float)((double)p->done / (double)p->total) : 0.0f;
        float build = p->nbp ? (float)p->nbuilt / (float)p->nbp : (p->state >= PRE_READY ? 1.0f : 0.0f);
        *progress = p->state == PRE_READY ? 1.0f : p->state == PRE_BUILDING ? 0.85f + 0.15f * build : 0.85f * read;
    }
    return (PreloadState)p->state;
}

void level_preload_cancel(const char* name) {
    bool all = !name || !name[0];
    Preload* one = all ? NULL : find(name);
    for (int i = 0; i < PRELOAD_MAX; i++)
        if (P.p[i].state != PRE_NONE && (all || &P.p[i] == one)) slot_free(&P.p[i]), P.st.cancelled++;
}

// The Blueprints' component trees of one preload, within ms (the main thread: the class registry)
static void build_some(Preload* p, double budget_ms) {
    if (p->thread) re_thread_join(p->thread), p->thread = NULL;
    double t0 = re_time_seconds();
    if (!p->cursor) { // (counted once: the progress)
        p->cursor = 1, p->nbp = 0;
        for (uint32_t s = 1; s < p->world.slot_count; s++) {
            Entity e = ecs_entity_at(&p->world, s);
            if (e && ecs_get(&p->world, e, COMP_BLUEPRINT) && !bp_is_component(&p->world, e)) p->nbp++;
        }
    }
    uint32_t end = p->world.slot_count; // (the trees made add rows past it: not visited)
    while (p->cursor < end) {
        Entity e = ecs_entity_at(&p->world, p->cursor++);
        CBlueprint* b = e ? ecs_get(&p->world, e, COMP_BLUEPRINT) : NULL;
        if (b && !bp_is_component(&p->world, e)) {
            b->inst = 0;
            bp_components_build(&p->world, e, NULL);
            p->nbuilt++;
        }
        if ((re_time_seconds() - t0) * 1000.0 >= budget_ms) break;
    }
    p->build_ms += (re_time_seconds() - t0) * 1000.0;
    if (p->cursor >= end) p->state = PRE_READY, P.st.last_read_ms = p->read_ms, P.st.last_build_ms = p->build_ms;
}

void level_preload_update(void) {
    ensure_init();
    double t0 = re_time_seconds();
    for (int i = 0; i < PRELOAD_MAX; i++) {
        Preload* p = &P.p[i];
        if (p->state != PRE_BUILDING) continue;
        float s = speed_of(p);
        build_some(p, 0.08 + (double)(s * s) * 8.0); // (0.02: ~0.1 ms a frame; 0.5: 2 ms; 1: 8 ms)
    }
    double ms = (re_time_seconds() - t0) * 1000.0;
    if (ms > P.st.frame_ms_max) P.st.frame_ms_max = ms;
}

bool level_preload_take(const char* path, World* w, LevelCamera* cam, void (*pump)(float progress), double* ms) {
    Preload* p = find(path);
    if (!p || p->state == PRE_FAILED) {
        if (p) slot_free(p);
        return false;
    }
    p->speed = 1.0f; // (the loading screen: as fast as it goes)
    while (p->state == PRE_READING || p->state == PRE_BUILDING) {
        if (p->state == PRE_BUILDING) build_some(p, 1e9);
        else re_sleep_seconds(0.002);
        if (pump) {
            float pr = 0.0f;
            level_preload_state(path, &pr);
            pump(pr);
        }
    }
    if (p->state != PRE_READY) {
        slot_free(p);
        return false;
    }
    double t0 = re_time_seconds();
    uint32_t rev = w->revision;
    const uint32_t* rel = w->relevant;
    uint32_t rels = w->relevant_slots;
    ecs_shutdown(w);
    *w = p->world; // (the rows move: a swap of pointers)
    w->revision = (rev > p->world.revision ? rev : p->world.revision) + 1; // (a bulk change: the renderer reads it all)
    w->relevant = rel, w->relevant_slots = rels;
    p->world_made = false;
    if (cam) *cam = p->cam;
    P.st.last_actors = w->alive;
    slot_free(p);
    P.st.taken++;
    double t = (re_time_seconds() - t0) * 1000.0;
    P.st.last_take_ms = t;
    if (ms) *ms = t;
    return true;
}

bool level_preload_path(const char* name, char* out, unsigned cap) {
    Preload* p = find(name);
    if (!p) return false;
    snprintf(out, cap, "%s", p->path);
    return true;
}

const PreloadStats* level_preload_stats(void) { return &P.st; }

#include "level_preload_test.inc" // (2026-11-23) --preload-test
