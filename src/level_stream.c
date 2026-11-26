// Level streaming and the Level Blueprint (level.h): Unreal's Load / Unload Stream Level and Open Level, shared by the
// editor's Play and the packaged game. A load reads the level's file on a thread (the frame goes on), then the main
// thread copies its actors into the world in one go (their Blueprints start); an unload destroys them. Each streamed
// actor carries CStreamed (its level's id).
#include "level.h"
#include "level_preload.h" // (2026-11-23) the async speed
#include <math.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "components.h"
#include "platform.h"
#include "platform_files.h"

#define MAX_REQ 64

typedef struct Req {
    uint32_t id;
    char name[128], path[700];
    bool load, visible;
    volatile int state; // 0 queued, 1 reading, 2 read (the file in data), 3 done, 4 failed
    volatile size_t at, total; // (2026-11-24) the bytes read so far and the file's: the Progress pin
    uint8_t* data;
    size_t size;
    ReThread* thread;
} Req;

static struct {
    char root[700];
    char persistent[128];
    Req reqs[MAX_REQ];
    uint32_t next_id;
    struct { char name[128]; uint32_t id; } loaded[32];
    uint32_t nloaded;
    char pending_open[700]; // Open Level: done between frames by the host
} L;

void level_stream_init(const char* content_root, const char* persistent_name) {
    for (int i = 0; i < MAX_REQ; i++) {
        if (L.reqs[i].thread) re_thread_join(L.reqs[i].thread);
        free(L.reqs[i].data);
    }
    memset(&L, 0, sizeof L);
    snprintf(L.root, sizeof L.root, "%s", content_root ? content_root : "Content");
    snprintf(L.persistent, sizeof L.persistent, "%s", persistent_name ? persistent_name : "");
    L.next_id = 1;
}

const char* level_stream_persistent(void) { return L.persistent; }

// Content/**/<name>.rlevel (the name with or without the extension, or a path)
typedef struct FindCtx { const char* want; char* out; size_t cap; bool found; } FindCtx;
static void find_one(void* user, const ReDirEntry* e);
static void find_in(const char* dir, FindCtx* c) {
    struct { FindCtx* c; const char* dir; } arg = { c, dir };
    re_dir_walk(dir, find_one, &arg);
}
static void find_one(void* user, const ReDirEntry* e) {
    struct { FindCtx* c; const char* dir; }* a = user;
    if (a->c->found) return;
    char p[900];
    snprintf(p, sizeof p, "%s/%s", a->dir, e->name);
    if (e->is_dir) {
        if (e->name[0] != '.') find_in(p, a->c);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot || _stricmp(dot, ".rlevel")) return;
    size_t n = (size_t)(dot - e->name);
    if (strlen(a->c->want) == n && !_strnicmp(e->name, a->c->want, n)) {
        snprintf(a->c->out, a->c->cap, "%s", p);
        a->c->found = true;
    }
}
bool level_find(const char* name, char* out, size_t cap) {
    if (!name || !name[0]) return false;
    bool is_dir;
    if (re_file_stat(name, NULL, NULL, &is_dir) && !is_dir) { // a path
        snprintf(out, cap, "%s", name);
        return true;
    }
    char want[128];
    const char* base = strrchr(name, '/');
    snprintf(want, sizeof want, "%s", base ? base + 1 : name);
    char* dot = strrchr(want, '.');
    if (dot && !_stricmp(dot, ".rlevel")) *dot = 0;
    FindCtx c = { want, out, cap, false };
    find_in(L.root, &c);
    return c.found;
}

static int read_thread(void* arg) {
    Req* r = arg;
    re_thread_set_low_priority(true);
    FILE* f = re_fopen(r->path, "rb");
    if (!f) {
        r->state = 4;
        return 0;
    }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = n > 0 ? malloc((size_t)n) : NULL; // a load on its thread, not a frame
    bool ok = d != NULL;
    r->total = n > 0 ? (size_t)n : 0;
    for (size_t at = 0; ok && at < (size_t)n;) { // (2026-11-23) in slices, paced by Set Async Loading Speed (1: no pause)
        double t0 = re_time_seconds();
        size_t want = (size_t)n - at < (256u << 10) ? (size_t)n - at : (256u << 10);
        ok = fread(d + at, 1, want, f) == want, at += want;
        r->at = at;
        float s = level_async_speed();
        if (s < 0.999f) re_sleep_seconds(fmin((re_time_seconds() - t0) * (1.0 / s - 1.0) + (1.0 - s) * 0.004, 0.06));
    }
    fclose(f);
    if (!ok) {
        free(d);
        r->state = 4;
        return 0;
    }
    r->data = d, r->size = (size_t)n;
    r->state = 2;
    return 0;
}

static int loaded_index(const char* name) {
    for (uint32_t i = 0; i < L.nloaded; i++)
        if (!_stricmp(L.loaded[i].name, name)) return (int)i;
    return -1;
}

uint32_t level_stream_request(const char* name, bool load, bool visible) {
    Req* r = NULL;
    for (int i = 0; i < MAX_REQ && !r; i++)
        if (!L.reqs[i].id || L.reqs[i].state >= 3) {
            if (L.reqs[i].thread) re_thread_join(L.reqs[i].thread), L.reqs[i].thread = NULL;
            free(L.reqs[i].data);
            memset(&L.reqs[i], 0, sizeof L.reqs[i]);
            r = &L.reqs[i];
        }
    if (!r) return 0;
    r->id = L.next_id++;
    r->load = load, r->visible = visible;
    snprintf(r->name, sizeof r->name, "%s", name ? name : "");
    char* dot = strrchr(r->name, '.');
    if (dot && !_stricmp(dot, ".rlevel")) *dot = 0;
    if (load) {
        if (loaded_index(r->name) >= 0) { // already in: done (Unreal's: nothing happens)
            r->state = 3;
            return r->id;
        }
        if (!level_find(r->name, r->path, sizeof r->path)) {
            printf("[RealEngine] Load Stream Level: no level \"%s\" in the project\n", r->name);
            r->state = 4;
            return r->id;
        }
        r->state = 1;
        r->thread = re_thread_start(read_thread, r);
        if (!r->thread) read_thread(r);
    } else r->state = 2; // an unload: done by the next update
    return r->id;
}

// (2026-11-24, item 8 of the list: the loading nodes' Loaded / Failed / Progress) A request's progress 0..1 (the file
// read: 0..0.95, its actors in: 1) and how it ended: 0 still going, 1 loaded, 2 failed. An id no longer listed: loaded
float level_stream_progress(uint32_t id, int* result) {
    for (int i = 0; i < MAX_REQ; i++) {
        const Req* r = &L.reqs[i];
        if (!id || r->id != id) continue;
        int st = r->state;
        if (result) *result = st == 3 ? 1 : st == 4 ? 2 : 0;
        if (st == 3) return 1.0f;
        if (st == 4) return 0.0f;
        if (st == 2) return 0.95f;
        return r->total ? 0.95f * (float)((double)r->at / (double)r->total) : 0.0f;
    }
    if (result) *result = id ? 1 : 2;
    return id ? 1.0f : 0.0f;
}
// The same by the level's name: its last request (none: in already = loaded, else not asked = 0 and 0)
float level_stream_state(const char* name, int* result) {
    char want[128];
    snprintf(want, sizeof want, "%s", name ? name : "");
    char* dot = strrchr(want, '.');
    if (dot && !_stricmp(dot, ".rlevel")) *dot = 0;
    uint32_t best = 0;
    for (int i = 0; i < MAX_REQ; i++)
        if (L.reqs[i].id > best && L.reqs[i].load && !_stricmp(L.reqs[i].name, want)) best = L.reqs[i].id;
    if (best) return level_stream_progress(best, result);
    bool in = loaded_index(want) >= 0;
    if (result) *result = in ? 1 : 0;
    return in ? 1.0f : 0.0f;
}

bool level_stream_done(uint32_t id) {
    for (int i = 0; i < MAX_REQ; i++)
        if (L.reqs[i].id == id) return L.reqs[i].state >= 3;
    return true;
}

// The level's actors into w (each its components; their Blueprints rebuild their component trees): its id on each
static uint32_t copy_level_id(World* w, const uint8_t* data, size_t size, const char* name, uint32_t want, bool visible, bool playing) {
    static World tmp;
    ecs_init(&tmp);
    bool ok = level_load_memory(&tmp, data, size, NULL);
    uint32_t id = 0;
    if (ok) {
        static uint32_t seq = 1000;
        id = want ? want : ++seq;
        uint32_t n = 0;
        for (uint32_t ai = 0; ai < tmp.archetype_count; ai++) {
            const EcsArchetype* a = &tmp.archetypes[ai];
            if (!a->count || (a->mask & COMP_BIT(COMP_ATTACH))) continue; // (a Blueprint's components: made again below)
            const Entity* ents = ecs_arch_entities(a);
            for (uint32_t i = 0; i < a->count; i++) {
                Entity e = ecs_create(w, a->mask | COMP_BIT(COMP_STREAMED));
                if (e == ENTITY_NONE) break;
                for (int c = 0; c < COMP_COUNT; c++)
                    if ((a->mask & COMP_BIT(c)) && c != COMP_STREAMED) memcpy(ecs_get(w, e, (ComponentId)c), ecs_get(&tmp, ents[i], (ComponentId)c), g_components[c].size);
                ((CStreamed*)ecs_get(w, e, COMP_STREAMED))->level = id;
                CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
                if (t && !visible) t->flags |= TRANSFORM_HIDDEN_GAME;
                CBlueprint* b = ecs_get(w, e, COMP_BLUEPRINT);
                if (b) {
                    b->inst = 0;
                    bp_components_build(w, e, NULL);
                    if (playing) bp_start_instance(w, e); // its BeginPlay
                }
                ecs_touch(w, e);
                n++;
            }
        }
        printf("[RealEngine] Stream level %s: %u actors in\n", name, n);
    }
    ecs_shutdown(&tmp);
    return id;
}

static uint32_t copy_level(World* w, const uint8_t* data, size_t size, const char* name, bool visible, bool playing) {
    return copy_level_id(w, data, size, name, 0, visible, playing);
}
// (2026-11-16, sublevels.c) a level's actors in under an id of the caller's; out by id; a level loaded by someone else
// made known to Load / Unload Stream Level by its name
uint32_t level_stream_copy(World* w, const uint8_t* data, size_t size, const char* name, uint32_t id, bool visible, bool playing) {
    return copy_level_id(w, data, size, name, id, visible, playing);
}
static void unload_level(World* w, uint32_t id);
void level_stream_unload_id(World* w, uint32_t id) { unload_level(w, id); }
void level_stream_register(const char* name, uint32_t id) {
    if (loaded_index(name) >= 0 || L.nloaded >= 32) return;
    snprintf(L.loaded[L.nloaded].name, sizeof L.loaded[0].name, "%s", name);
    L.loaded[L.nloaded++].id = id;
}
void level_stream_forget(const char* name) {
    int k = loaded_index(name);
    if (k >= 0) L.loaded[k] = L.loaded[--L.nloaded];
}
static void unload_level(World* w, uint32_t id) {
    uint32_t n = 0;
    for (int pass = 0; pass < 8; pass++) { // (destroying moves rows: until none is left)
        bool any = false;
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!(a->mask & COMP_BIT(COMP_STREAMED)) || !a->count) continue;
            const CStreamed* s = ecs_arch_column(a, COMP_STREAMED);
            const Entity* ents = ecs_arch_entities(a);
            for (uint32_t i = a->count; i-- > 0;)
                if (s[i].level == id) ecs_destroy(w, ents[i]), n++, any = true;
        }
        if (!any) break;
    }
    printf("[RealEngine] Stream level unloaded: %u actors out\n", n);
}

void level_stream_update(World* w, bool playing) {
    for (int i = 0; i < MAX_REQ; i++) {
        Req* r = &L.reqs[i];
        if (!r->id || r->state != 2) continue;
        if (r->load) {
            if (r->thread) re_thread_join(r->thread), r->thread = NULL;
            uint32_t id = copy_level(w, r->data, r->size, r->name, r->visible, playing);
            free(r->data), r->data = NULL;
            if (id && L.nloaded < 32) {
                snprintf(L.loaded[L.nloaded].name, sizeof L.loaded[0].name, "%s", r->name);
                L.loaded[L.nloaded++].id = id;
            }
            r->state = id ? 3 : 4;
        } else {
            int k = loaded_index(r->name);
            if (k >= 0) {
                unload_level(w, L.loaded[k].id);
                L.loaded[k] = L.loaded[--L.nloaded];
            }
            r->state = 3;
        }
    }
}

void level_open_request(const char* name) {
    if (!level_find(name, L.pending_open, sizeof L.pending_open)) {
        printf("[RealEngine] Open Level: no level \"%s\" in the project\n", name);
        L.pending_open[0] = 0;
    }
}
const char* level_open_pending(void) { return L.pending_open[0] ? L.pending_open : NULL; }
void level_open_clear(void) { L.pending_open[0] = 0; }

// --- The Level Blueprint: <level>.rlevelbp next to it (a Blueprint Class whose parent is Level Script Actor) ---

void level_bp_path(const char* level_path, char* out, size_t cap) {
    snprintf(out, cap, "%s", level_path);
    char* dot = strrchr(out, '.');
    size_t at = dot ? (size_t)(dot - out) : strlen(out);
    snprintf(out + at, cap - at, ".rlevelbp");
}

// The vpath ("/Content/...") of a disk path under the Content folder (false: outside it)
static bool to_vpath(const char* path, char* out, size_t cap) {
    size_t n = strlen(L.root);
    if (_strnicmp(path, L.root, n)) return false;
    snprintf(out, cap, "/Content%s", path + n);
    for (char* c = out; *c; c++)
        if (*c == '\\') *c = '/';
    return true;
}

Entity level_bp_spawn(World* w, const char* level_path) {
    if (!level_path || !level_path[0]) return ENTITY_NONE;
    char bp[760], vp[800];
    level_bp_path(level_path, bp, sizeof bp);
    if (!re_file_stat(bp, NULL, NULL, NULL) || !to_vpath(bp, vp, sizeof vp)) return ENTITY_NONE;
    BpAsset* cls = bp_load(vp);
    if (!cls) return ENTITY_NONE;
    Entity e = bp_spawn(w, cls, (float[3]){ 0, 0, 0 }, (float[3]){ 0, 0, 0 });
    CName* n = e ? ecs_get(w, e, COMP_NAME) : NULL;
    if (n) snprintf(n->text, sizeof n->text, "LevelScriptActor");
    return e;
}

BpAsset* level_bp_open_or_create(const char* level_path) {
    char bp[760], vp[800];
    level_bp_path(level_path, bp, sizeof bp);
    if (!to_vpath(bp, vp, sizeof vp)) return NULL;
    if (re_file_stat(bp, NULL, NULL, NULL)) return bp_load(vp);
    char folder[800], name[200];
    snprintf(folder, sizeof folder, "%s", vp);
    char* slash = strrchr(folder, '/');
    if (!slash) return NULL;
    *slash = 0;
    snprintf(name, sizeof name, "%s", slash + 1);
    char* dot = strrchr(name, '.');
    if (dot) *dot = 0;
    // made as a class (.rbp), then its file renamed to the level's (bp_create names files by kind)
    BpAsset* a = bp_create(BPA_CLASS, folder, name, "Level Script Actor");
    if (!a) return NULL;
    char made[700];
    snprintf(made, sizeof made, "%s", a->path);
    if (re_move_path(made, bp)) {
        snprintf(a->path, sizeof a->path, "%s", bp);
        a->hash = bp_vpath_hash(vp);
    }
    return a;
}
