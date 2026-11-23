// Sublevels and Lighting Levels (sublevels.h, notas-dev/niveis.md)
#include "sublevels.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"
#include "async.h"
#include "gi_bake.h"
#include "lights.h"
#include "level.h"
#include "platform.h"
#include "platform_files.h"

static SublevelSet* g_active;
static struct VkCtx* g_vk; // (the game's or the editor's: Set Lighting Level Visible from a Blueprint has none)
static char g_active_level[700];
SublevelSet* sublevels_active(void) { return g_active; }
void sublevels_set_active(SublevelSet* set, const char* level_path) {
    g_active = set;
    snprintf(g_active_level, sizeof g_active_level, "%s", level_path ? level_path : "");
}

void sublevels_list_path(const char* level_path, char* out, size_t cap) {
    snprintf(out, cap, "%s", level_path);
    char* dot = strrchr(out, '.');
    if (dot && !strchr(dot, '\\') && !strchr(dot, '/')) *dot = 0;
    strncat(out, ".rlevels", cap - strlen(out) - 1);
}

// a swatch per slot (the Levels panel, as Unreal's level colors)
static void default_color(uint32_t i, float c[3]) {
    static const float pal[8][3] = { { 0.95f, 0.55f, 0.2f }, { 0.3f, 0.75f, 0.95f }, { 0.55f, 0.9f, 0.35f }, { 0.9f, 0.4f, 0.75f },
                                     { 0.95f, 0.85f, 0.3f }, { 0.5f, 0.5f, 0.95f }, { 0.35f, 0.9f, 0.8f }, { 0.9f, 0.35f, 0.35f } };
    memcpy(c, pal[i & 7u], sizeof(float) * 3);
}

bool sublevels_read(const char* level_path, SublevelSet* set) {
    memset(set, 0, sizeof *set);
    set->current = -1;
    char p[760];
    sublevels_list_path(level_path, p, sizeof p);
    FILE* f = re_fopen(p, "rb");
    if (!f) return false;
    char line[1024];
    while (fgets(line, sizeof line, f) && set->n < SUBLEVEL_MAX) {
        char* e = line + strlen(line);
        while (e > line && (e[-1] == '\n' || e[-1] == '\r' || e[-1] == ' ')) *--e = 0;
        if (!line[0] || line[0] == '#') continue;
        if (!strncmp(line, "current=", 8)) {
            set->current = atoi(line + 8);
            continue;
        }
        Sublevel* s = &set->s[set->n];
        memset(s, 0, sizeof *s);
        default_color(set->n, s->color);
        int vis = 1, init = 1, lit = 0, lock = 0;
        float wt = 1.0f;
        char name[96] = { 0 };
        float r = s->color[0], g = s->color[1], b = s->color[2];
        // name|visible|initially loaded|lighting|locked|r g b
        int got = sscanf(line, "%95[^|]|%d|%d|%d|%d|%f %f %f|%f", name, &vis, &init, &lit, &lock, &r, &g, &b, &wt);
        if (got < 1 || !name[0]) continue;
        snprintf(s->name, sizeof s->name, "%s", name);
        s->visible = vis != 0, s->initially_loaded = init != 0, s->lighting = lit != 0, s->locked = lock != 0;
        s->color[0] = r, s->color[1] = g, s->color[2] = b;
        s->weight = wt >= 0.0f ? wt : 1.0f, s->layer = -1;
        if (!level_find(s->name, s->path, sizeof s->path)) s->path[0] = 0;
        set->n++;
    }
    fclose(f);
    if (set->current >= (int)set->n) set->current = -1;
    return true;
}

bool sublevels_write(const char* level_path, const SublevelSet* set) {
    char p[760];
    sublevels_list_path(level_path, p, sizeof p);
    if (!set->n) {
        re_remove_file(p);
        return true;
    }
    char tmp[800];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE* f = re_fopen(tmp, "wb");
    if (!f) return false;
    fprintf(f, "# Real Engine sublevels (Window > Levels): name|visible|initially loaded|lighting level|locked|color|lighting blend\n");
    for (uint32_t i = 0; i < set->n; i++) {
        const Sublevel* s = &set->s[i];
        fprintf(f, "%s|%d|%d|%d|%d|%.3f %.3f %.3f|%.3f\n", s->name, s->visible, s->initially_loaded, s->lighting, s->locked, s->color[0], s->color[1], s->color[2], s->weight);
    }
    fprintf(f, "current=%d\n", set->current);
    bool ok = fclose(f) == 0;
    return ok && re_replace_file(tmp, p);
}

// The actor an entity belongs to: up its owners (a Blueprint's components, a skeletal mesh's parts)
static Entity owner_root(World* w, Entity e) {
    for (int k = 0; k < 8; k++) {
        const CAttach* a = ecs_get(w, e, COMP_ATTACH);
        if (!a || !a->owner || !ecs_alive(w, a->owner)) break;
        e = a->owner;
    }
    return e;
}
static uint32_t level_id_of(World* w, Entity e) {
    const CStreamed* s = ecs_get(w, owner_root(w, e), COMP_STREAMED);
    return s ? s->level : 0;
}

static bool read_file(const char* path, uint8_t** data, size_t* size) {
    FILE* f = re_fopen(path, "rb");
    if (!f) return false;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t* d = n > 0 ? malloc((size_t)n) : NULL; // a level load, not a frame
    bool ok = d && fread(d, 1, (size_t)n, f) == (size_t)n;
    fclose(f);
    if (!ok) {
        free(d);
        return false;
    }
    *data = d, *size = (size_t)n;
    return true;
}

static void set_hidden(World* w, uint32_t id, uint32_t flag, bool hidden) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            if (level_id_of(w, ents[i]) != id) continue;
            uint32_t old = t[i].flags; // (an actor hidden by itself stays hidden: only what the level hid is shown again)
            if (hidden && !(old & flag)) t[i].flags = old | flag | TRANSFORM_LEVEL_HIDDEN;
            else if (!hidden && (old & TRANSFORM_LEVEL_HIDDEN)) t[i].flags = old & ~(flag | TRANSFORM_LEVEL_HIDDEN);
            if (t[i].flags != old) ecs_touch(w, ents[i]);
        }
    }
}

bool sublevel_load(World* w, SublevelSet* set, uint32_t slot) {
    if (slot >= set->n) return false;
    Sublevel* s = &set->s[slot];
    if (s->id) return true;
    if (!s->path[0] && !level_find(s->name, s->path, sizeof s->path)) {
        printf("[RealEngine] Sublevel %s: no such level in the project\n", s->name);
        return false;
    }
    uint8_t* data = NULL;
    size_t size = 0;
    if (!read_file(s->path, &data, &size)) return false;
    uint32_t id = level_stream_copy(w, data, size, s->name, SUBLEVEL_ID0 + slot, true, false);
    free(data);
    if (!id) return false;
    s->id = id;
    if (!s->visible) set_hidden(w, id, TRANSFORM_HIDDEN, true);
    return true;
}

void sublevel_unload(World* w, SublevelSet* set, uint32_t slot) {
    if (slot >= set->n || !set->s[slot].id) return;
    uint32_t id = set->s[slot].id;
    for (int pass = 0; pass < 8; pass++) { // (its actors and what they own; destroying moves rows: until none is left)
        bool any = false;
        for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
            const EcsArchetype* a = &w->archetypes[ai];
            if (!a->count) continue;
            const Entity* ents = ecs_arch_entities(a);
            for (uint32_t i = a->count; i-- > 0;)
                if (level_id_of(w, ents[i]) == id) ecs_destroy(w, ents[i]), any = true;
        }
        if (!any) break;
    }
    set->s[slot].id = 0;
}

void sublevel_set_visible(World* w, Sublevel* s, bool visible, bool game) {
    s->visible = visible;
    if (!s->id) return;
    set_hidden(w, s->id, game ? TRANSFORM_HIDDEN_GAME : TRANSFORM_HIDDEN, !visible);
}

int sublevel_of(World* w, const SublevelSet* set, Entity e) {
    uint32_t id = level_id_of(w, e);
    for (uint32_t i = 0; i < set->n; i++)
        if (set->s[i].id && set->s[i].id == id) return (int)i;
    return -1;
}

static void mark_level(World* w, Entity e, uint32_t id) {
    if (!id) {
        if (ecs_get(w, e, COMP_STREAMED)) ecs_remove(w, e, COMP_STREAMED);
        return;
    }
    if (!ecs_get(w, e, COMP_STREAMED) && !ecs_add(w, e, COMP_STREAMED)) return;
    ((CStreamed*)ecs_get(w, e, COMP_STREAMED))->level = id;
    ecs_touch(w, e);
}
void sublevel_move_actor(World* w, const SublevelSet* set, Entity e, int slot) {
    e = owner_root(w, e);
    uint32_t id = slot >= 0 && slot < (int)set->n ? set->s[slot].id : 0;
    // what it owns first (a Blueprint's components, a skeletal mesh's parts: the streaming's Unload takes them by their
    // own mark); adding or removing a component moves rows, so a list taken before
    static Entity owned[4096];
    uint32_t no = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_ATTACH))) continue;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && no < 4096; i++)
            if (ents[i] != e && owner_root(w, ents[i]) == e) owned[no++] = ents[i];
    }
    for (uint32_t i = 0; i < no; i++) mark_level(w, owned[i], id);
    mark_level(w, e, id);
}

size_t sublevels_count_actors(World* w, const Sublevel* s) {
    if (!s->id) return 0;
    size_t n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_STREAMED)) || (a->mask & COMP_BIT(COMP_ATTACH))) continue;
        const CStreamed* st = ecs_arch_column(a, COMP_STREAMED);
        for (uint32_t i = 0; i < a->count; i++) n += st[i].level == s->id;
    }
    return n;
}

// A copy of the world (the same handles: the owners stay right) with only what keep() says, then saved
static World g_tmp;
static bool save_filtered(World* w, const LevelCamera* cam, const char* path, size_t* bytes, uint32_t keep_id, bool keep_unmarked) {
    size_t n = ecs_serialized_size(w);
    uint8_t* blob = malloc(n); // a save, not a frame
    if (!blob) return false;
    ecs_serialize(w, blob);
    ecs_init(&g_tmp);
    bool ok = ecs_deserialize(&g_tmp, blob, n);
    free(blob);
    if (ok) {
        for (int pass = 0; pass < 8; pass++) {
            bool any = false;
            for (uint32_t ai = 0; ai < g_tmp.archetype_count; ai++) {
                const EcsArchetype* a = &g_tmp.archetypes[ai];
                if (!a->count) continue;
                const Entity* ents = ecs_arch_entities(a);
                for (uint32_t i = a->count; i-- > 0;) {
                    uint32_t id = level_id_of(&g_tmp, ents[i]);
                    bool keep = keep_unmarked ? id == 0 : id == keep_id;
                    if (!keep) ecs_destroy(&g_tmp, ents[i]), any = true;
                }
            }
            if (!any) break;
        }
        // what a level's eye or a bake hid is saved shown
        for (uint32_t ai = 0; ai < g_tmp.archetype_count; ai++) {
            const EcsArchetype* a = &g_tmp.archetypes[ai];
            if (!a->count || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
            CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
            for (uint32_t i = 0; i < a->count; i++)
                if (t[i].flags & TRANSFORM_LEVEL_HIDDEN) t[i].flags &= ~(TRANSFORM_HIDDEN | TRANSFORM_HIDDEN_GAME | TRANSFORM_LEVEL_HIDDEN);
        }
        // a sublevel's actors lose their mark (opened alone, it is a level like any other)
        for (int pass = 0; pass < 4 && !keep_unmarked; pass++) {
            bool any = false;
            for (uint32_t ai = 0; ai < g_tmp.archetype_count; ai++) {
                const EcsArchetype* a = &g_tmp.archetypes[ai];
                if (!a->count || !(a->mask & COMP_BIT(COMP_STREAMED))) continue;
                const Entity* ents = ecs_arch_entities(a);
                for (uint32_t i = a->count; i-- > 0;) ecs_remove(&g_tmp, ents[i], COMP_STREAMED), any = true;
            }
            if (!any) break;
        }
        LevelCamera c0 = { { 0, 0, 200 }, 0, 0, 90, 600 };
        ok = level_save(&g_tmp, cam ? cam : &c0, path, bytes);
    }
    ecs_shutdown(&g_tmp);
    return ok;
}

bool sublevel_save(World* w, const Sublevel* s) {
    if (!s->id || !s->path[0]) return false;
    return save_filtered(w, NULL, s->path, NULL, s->id, false);
}

bool level_save_without_sublevels(World* w, const LevelCamera* cam, const char* path, size_t* bytes) {
    return save_filtered(w, cam, path, bytes, 0, true);
}

static float effective(const Sublevel* s) { return s->id && s->visible ? s->weight : 0.0f; }

// a level's lights at its weight (lights.c scales them; touched: the light buffer takes them again)
static void touch_lights(World* w, uint32_t id) {
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_LIGHT))) continue;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (id == UINT32_MAX || level_id_of(w, ents[i]) == id) ecs_touch(w, ents[i]);
    }
}
// (2026-11-17, Q3) the weight is the level's whole share of the light: its lights, its sky light, its fog and clouds
// (weather.c), its Post Process Volumes (post.c) and its bake's layer; 0 when it is off (in the game too, where the
// editor's eye does nothing). A sublevel that isn't a Lighting Level stays at 1 (its eye hides it)
void sublevels_apply_weight(World* w, Sublevel* s) {
    if (!s->id) return;
    float e = s->lighting ? effective(s) : 1.0f;
    lights_set_level_weight(s->id, e);
    if (w) touch_lights(w, s->id);
    if (s->layer >= 0) gi_lightmaps_layer_weight(s->layer, e);
}
void sublevels_apply_all(World* w, SublevelSet* set) {
    for (uint32_t id = SUBLEVEL_ID0; id < SUBLEVEL_ID0 + SUBLEVEL_MAX; id++) lights_set_level_weight(id, 1.0f); // (a slot left by a removed level)
    for (uint32_t i = 0; set && i < set->n; i++)
        if (set->s[i].id) sublevels_apply_weight(NULL, &set->s[i]); // (touched once below)
    if (w) touch_lights(w, UINT32_MAX);
}
void sublevels_bake_weights(World* w, const SublevelSet* set) { // (a bake sees each level as it is, at 1: the blend comes after)
    for (uint32_t i = 0; set && i < set->n; i++)
        if (set->s[i].id) lights_set_level_weight(set->s[i].id, 1.0f);
    if (w) touch_lights(w, UINT32_MAX);
}

bool sublevels_load_lighting(struct VkCtx* vk, const char* level_path, SublevelSet* set) {
    static char paths[SUBLEVEL_MAX + 1][760];
    const char* list[SUBLEVEL_MAX + 1];
    float wts[SUBLEVEL_MAX + 1];
    int slot_of[SUBLEVEL_MAX + 1], layer_of[SUBLEVEL_MAX + 1];
    int n = 0;
    level_rlight_path(level_path, paths[n], sizeof paths[0]);
    if (re_file_stat(paths[n], NULL, NULL, NULL)) list[n] = paths[n], wts[n] = 1.0f, slot_of[n] = -1, n++;
    for (uint32_t i = 0; set && i < set->n; i++) {
        Sublevel* s = &set->s[i];
        s->layer = -1;
        if (!s->lighting || !s->path[0]) continue;
        level_rlight_path(s->path, paths[n], sizeof paths[0]); // (every Lighting Level's, on or off: switching is then free)
        if (re_file_stat(paths[n], NULL, NULL, NULL)) list[n] = paths[n], wts[n] = effective(s), slot_of[n] = (int)i, n++;
    }
    if (!n) {
        gi_lightmaps_clear(vk);
        return false;
    }
    int layers = gi_lightmaps_load_layers(vk, list, n, wts, layer_of);
    for (int k = 0; k < n; k++)
        if (slot_of[k] >= 0) set->s[slot_of[k]].layer = layer_of[k];
    for (uint32_t i = 0; set && i < set->n; i++)
        if (set->s[i].id) lights_set_level_weight(set->s[i].id, set->s[i].lighting ? effective(&set->s[i]) : 1.0f);
    return layers > 0;
}

// (2026-11-17, Q3) what lights a level: its lights (sun, sky light...), its fog, local fog volumes, clouds and Post
// Process Volumes. A bake of one Lighting Level sees only its own (a sky preset made in it: its sun, sky, clouds, fog)
#define LIGHTING_ACTORS (COMP_BIT(COMP_LIGHT) | COMP_BIT(COMP_FOG) | COMP_BIT(COMP_FOG_VOLUME) | COMP_BIT(COMP_CLOUD) | COMP_BIT(COMP_POST_PROCESS))
void sublevels_lights_only(World* w, const SublevelSet* set, int lighting_slot, LightMask* saved) {
    saved->n = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & LIGHTING_ACTORS) || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        CTransform* t = ecs_arch_column(a, COMP_TRANSFORM);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count && saved->n < 1024; i++) {
            int slot = sublevel_of(w, set, ents[i]);
            bool in_lighting = slot >= 0 && set->s[slot].lighting;
            bool keep = lighting_slot < 0 ? !in_lighting : slot == lighting_slot;
            if (keep || (t[i].flags & TRANSFORM_HIDDEN)) continue;
            saved->e[saved->n] = ents[i], saved->flags[saved->n++] = t[i].flags;
            t[i].flags |= TRANSFORM_HIDDEN | TRANSFORM_LEVEL_HIDDEN;
            ecs_touch(w, ents[i]);
        }
    }
}
void sublevels_lights_restore(World* w, const LightMask* saved) {
    for (uint32_t i = 0; i < saved->n; i++) {
        CTransform* t = ecs_get(w, saved->e[i], COMP_TRANSFORM);
        if (t) t->flags = saved->flags[i], ecs_touch(w, saved->e[i]);
    }
}

void sublevels_begin_game(struct VkCtx* vk, World* w, const char* level_path, SublevelSet* set) {
    if (vk) g_vk = vk;
    sublevels_set_active(set, level_path);
    for (uint32_t i = 0; i < set->n; i++) {
        Sublevel* s = &set->s[i];
        if (!s->initially_loaded) {
            sublevel_unload(w, set, i); // (the Play: the editor had it in)
            continue;
        }
        if (!s->id) sublevel_load(w, set, i);
        if (s->id) {
            set_hidden(w, s->id, TRANSFORM_HIDDEN, false); // (the editor's eye is the editor's)
            level_stream_register(s->name, s->id);
        }
        s->visible = true;
    }
    bool lit = false;
    for (uint32_t i = 0; i < set->n; i++) lit |= set->s[i].lighting;
    if (vk && lit) sublevels_load_lighting(vk, level_path, set);
    sublevels_apply_all(w, set);
}

// a level not loaded, read on a worker (the game: no hitch); its actors in on the main thread
typedef struct LoadJob { char path[760], name[96]; uint8_t* data; size_t size; bool game; } LoadJob;
static World* g_job_world;
static void load_work(AsyncTask* t, void* arg) {
    (void)t;
    LoadJob* j = arg;
    FILE* f = re_fopen(j->path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    j->data = n > 0 ? malloc((size_t)n) : NULL; // (a worker's private memory)
    if (j->data && fread(j->data, 1, (size_t)n, f) == (size_t)n) j->size = (size_t)n;
    fclose(f);
}
static void load_done(AsyncTask* t, void* arg, bool ok) {
    (void)t;
    LoadJob* j = arg;
    SublevelSet* set = g_active;
    for (uint32_t i = 0; ok && set && j->size && i < set->n; i++) {
        Sublevel* s = &set->s[i];
        if (_stricmp(s->name, j->name) || s->id) continue;
        uint32_t id = level_stream_copy(g_job_world, j->data, j->size, s->name, SUBLEVEL_ID0 + i, true, j->game);
        if (!id) break;
        s->id = id;
        if (j->game) level_stream_register(s->name, id);
        sublevel_set_visible(g_job_world, s, s->visible, j->game);
        sublevels_apply_weight(g_job_world, s);
        break;
    }
    free(j->data);
    free(j);
}

bool sublevels_set_lighting_blend(struct VkCtx* vk, World* w, const char* name, float weight, bool game) {
    SublevelSet* set = g_active;
    if (!set || !name) return false;
    if (!vk) vk = g_vk;
    for (uint32_t i = 0; i < set->n; i++) {
        Sublevel* s = &set->s[i];
        if (!s->lighting || _stricmp(s->name, name)) continue;
        if (weight < 0.0f) weight = 0.0f;
        bool on = weight > 0.0f;
        s->weight = weight;
        if (s->visible != on) sublevel_set_visible(w, s, on, game);
        s->visible = on;
        if (on && !s->id) { // (not in yet: read on a worker, in next frames)
            if (async_ready() && s->path[0]) {
                LoadJob* j = calloc(1, sizeof *j); // (freed by load_done)
                if (j) {
                    snprintf(j->path, sizeof j->path, "%s", s->path), snprintf(j->name, sizeof j->name, "%s", s->name), j->game = game;
                    g_job_world = w;
                    async_run(s->name, load_work, load_done, j, ASYNC_HIDDEN);
                }
            } else if (sublevel_load(w, set, i)) {
                if (game) level_stream_register(s->name, s->id);
            }
        }
        if (s->layer < 0 && vk && gi_lightmaps_layers() < 2) sublevels_load_lighting(vk, g_active_level, set); // (the layers once)
        sublevels_apply_weight(w, s);
        return true;
    }
    return false;
}

bool sublevels_set_lighting_visible(struct VkCtx* vk, World* w, const char* name, bool visible, bool game) {
    SublevelSet* set = g_active;
    if (!set || !name) return false;
    for (uint32_t i = 0; i < set->n; i++) {
        Sublevel* s = &set->s[i];
        if (!s->lighting || _stricmp(s->name, name)) continue;
        float keep = s->weight > 0.0f ? s->weight : 1.0f; // (off keeps its blend for when it comes back on)
        bool ok = sublevels_set_lighting_blend(vk, w, name, visible ? keep : 0.0f, game);
        s->weight = keep;
        return ok;
    }
    return false;
}

float sublevels_lighting_blend(const char* name) {
    if (!g_active || !name) return 0.0f;
    for (uint32_t i = 0; i < g_active->n; i++) {
        const Sublevel* s = &g_active->s[i];
        if (s->lighting && !_stricmp(s->name, name)) return effective(s);
    }
    return 0.0f;
}

bool sublevels_lighting_visible(const char* name) {
    if (!g_active || !name) return false;
    for (uint32_t i = 0; i < g_active->n; i++) {
        const Sublevel* s = &g_active->s[i];
        if (s->lighting && !_stricmp(s->name, name)) return s->visible && s->id;
    }
    return false;
}
