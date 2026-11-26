// Window > Levels (2026-11-16, notas-dev/niveis.md): Unreal's Levels panel. The persistent level and its sublevels
// (sublevels.h): each shown or hidden (the eye), locked, loaded at the start of the game or not, the current one (where
// new actors go), a Lighting Level or not. Lighting Levels stack: Build Lighting bakes the persistent level's lights,
// then each lighting level's alone, and what is shown is the sum of the visible ones (Unreal's Lighting Scenarios: one).
#include "editor.h"
#include "ui_number.h" // (2026-11-25, item 20: a typed value past the slider is kept)

#include "theme.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>

#include "async.h"
#include "blueprint.h"
#include "components.h"
#include "content.h"
#include "gi.h"
#include "gi_bake.h"
#include "i18n.h"
#include "level.h"
#include "platform.h"
#include "platform_files.h"
#include "lights.h"
#include "sublevels.h"
#include "ui_icons.h"

static SublevelSet g_set;      // the open level's sublevels
static SublevelSet g_play_set; // the Play's (begun from g_set; the editor's comes back at Stop)
static bool g_show;
// (2026-11-17, Q3) the level picked in the panel: the Outliner shows only its actors (LEVELS_ALL: every actor)
#define LEVELS_ALL (-2)
static int g_view = LEVELS_ALL;
static uint32_t g_view_rev = 1; // (the Outliner's list made again when it changes)
static int g_confirm_remove = -1; // the sublevel the "Remove?" dialog asks about
// (2026-11-19, the panel's test: levels_q3_test.c, REALENGINE_LEVELS_TEST=panel) where each card's controls were drawn
// last frame - the test clicks them by injected mouse events - and a hook called at the top of each frame
enum { LV_R_CARD, LV_R_EYE, LV_R_KIND, LV_R_BLEND, LV_R_START, LV_R_LOCK, LV_R_TRASH, LV_R_N };
static float g_rects[SUBLEVEL_MAX + 1][LV_R_N][4];
void (*levels_test_hook)(Editor* ed, double now);
static void rect_rec(int slot, int what) {
    if (!levels_test_hook) return;
    ImVec2_c a = igGetItemRectMin(), b = igGetItemRectMax();
    float* r = g_rects[slot + 1][what];
    r[0] = a.x, r[1] = a.y, r[2] = b.x, r[3] = b.y;
}
bool levels_test_rect(int slot, int what, float out[4]) {
    if (slot < -1 || slot >= SUBLEVEL_MAX || what < 0 || what >= LV_R_N) return false;
    memcpy(out, g_rects[slot + 1][what], sizeof(float) * 4);
    return out[2] > out[0];
}
bool levels_test_confirm_open(void) { return g_confirm_remove >= 0; }

// --- New actors: into the current level ---
// An actor seen at an index before (the entity there): a new one is the one not seen, given the current level's mark.
// Only walked when the current level is a sublevel and the actor count changed.
static Entity* g_known;
static uint32_t g_known_alive = UINT32_MAX;
static void known_sync(World* w) {
    if (!g_known && g_set.current >= 0 && g_set.current < (int)g_set.n) g_known = calloc(RE_ECS_MAX_ENTITIES, sizeof(Entity)); // once (4 MB): a sublevel made current
    if (!g_known) return;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) g_known[entity_index(ents[i])] = ents[i];
    }
    g_known_alive = w->alive;
}
void levels_resync(Editor* ed) { known_sync(&ed->world); }

static void assign_new_actors(Editor* ed) {
    World* w = &ed->world;
    if (g_set.current < 0 || g_set.current >= (int)g_set.n || !g_set.s[g_set.current].id) return;
    if (!g_known) {
        g_known = calloc(RE_ECS_MAX_ENTITIES, sizeof(Entity)); // once (4 MB), the first time a sublevel is current
        if (!g_known) return;
        known_sync(w);
        return;
    }
    if (w->alive == g_known_alive) return;
    static Entity fresh[4096];
    uint32_t nf = 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_TRANSFORM))) continue;
        const Entity* ents = ecs_arch_entities(a);
        bool marked = (a->mask & COMP_BIT(COMP_STREAMED)) != 0, owned = (a->mask & COMP_BIT(COMP_ATTACH)) != 0;
        for (uint32_t i = 0; i < a->count; i++) {
            uint32_t k = entity_index(ents[i]);
            if (g_known[k] == ents[i]) continue;
            g_known[k] = ents[i];
            if (!marked && !owned && nf < 4096) fresh[nf++] = ents[i]; // (a duplicate keeps its level's mark)
        }
    }
    for (uint32_t i = 0; i < nf; i++)
        if (ecs_alive(w, fresh[i])) sublevel_move_actor(w, &g_set, fresh[i], g_set.current);
    if (nf) g_set.s[g_set.current].dirty = true;
    g_known_alive = w->alive;
}

// --- The open level ---

static bool any_lighting(void) {
    for (uint32_t i = 0; i < g_set.n; i++)
        if (g_set.s[i].lighting) return true;
    return false;
}
bool levels_lighting_summed(void) { return any_lighting(); }

static void show_lighting(Editor* ed) {
    if (!ed->level_path[0]) return;
    if (!sublevels_load_lighting(ed->vk, ed->level_path, &g_set)) gi_lightmaps_clear(ed->vk);
    sublevels_apply_all(&ed->world, &g_set); // (2026-11-17: every level's weight: its lights, sky, fog, clouds, Post Process)
    gi_reset_probes();
}
static void set_view(int v) {
    if (v != g_view) g_view = v, g_view_rev++;
}

void levels_on_open(Editor* ed) {
    sublevels_read(ed->level_path, &g_set);
    for (uint32_t i = 0; i < g_set.n; i++) sublevel_load(&ed->world, &g_set, i);
    sublevels_set_active(&g_set, ed->level_path);
    bp_components_rebuild(&ed->world, NULL);
    if (g_set.n) show_lighting(ed);
    else sublevels_apply_all(&ed->world, &g_set); // (another level's weights gone)
    set_view(LEVELS_ALL);
    g_confirm_remove = -1;
    known_sync(&ed->world);
}
void levels_on_new(Editor* ed) {
    memset(&g_set, 0, sizeof g_set);
    g_set.current = -1;
    sublevels_set_active(&g_set, "");
    sublevels_apply_all(&ed->world, &g_set);
    set_view(LEVELS_ALL);
    g_confirm_remove = -1;
    known_sync(&ed->world);
}

bool levels_in_current(Editor* ed, Entity e) {
    if (!g_set.n) return true;
    return sublevel_of(&ed->world, &g_set, e) == g_set.current;
}

// --- The Outliner by level (2026-11-17, Q3: "ao selecionar um nivel, so os atores dele") ---
uint32_t levels_outliner_revision(void) { return g_set.n ? g_view_rev : 0; }
bool levels_outliner_shows(Editor* ed, Entity e) {
    if (!g_set.n || g_view == LEVELS_ALL || g_view >= (int)g_set.n) return true;
    return sublevel_of(&ed->world, &g_set, e) == g_view;
}
int levels_outliner_level(void) { return g_set.n ? g_view : LEVELS_ALL; }
void levels_outliner_set_level(int v) { set_view(v < -1 || v >= (int)g_set.n ? LEVELS_ALL : v); }
static const char* view_name(int v) {
    return v == LEVELS_ALL ? TR("All Levels") : v < 0 ? TR("Persistent Level") : g_set.s[v].name;
}
void levels_outliner_header(Editor* ed) {
    (void)ed;
    if (!g_set.n) return;
    if (g_view >= (int)g_set.n) set_view(LEVELS_ALL);
    ImVec4 col = g_view >= 0 ? (ImVec4){ g_set.s[g_view].color[0], g_set.s[g_view].color[1], g_set.s[g_view].color[2], 1.0f }
                             : igGetStyle()->Colors[ImGuiCol_TextDisabled];
    ui_icon_item(ICON_LEVEL, igGetFontSize(), igColorConvertFloat4ToU32(col));
    igSameLine(0, 6);
    igTextUnformatted(TR("Level:"), NULL);
    igSameLine(0, 6);
    igSetNextItemWidth(-1);
    if (igBeginCombo("##ol_level", view_name(g_view), 0)) {
        if (igSelectable_Bool(TR("All Levels"), g_view == LEVELS_ALL, 0, (ImVec2){ 0, 0 })) set_view(LEVELS_ALL);
        if (igSelectable_Bool(TR("Persistent Level"), g_view == -1, 0, (ImVec2){ 0, 0 })) set_view(-1);
        for (uint32_t i = 0; i < g_set.n; i++) {
            igPushID_Int((int)i);
            if (igSelectable_Bool(g_set.s[i].name, g_view == (int)i, 0, (ImVec2){ 0, 0 })) set_view((int)i);
            igPopID();
        }
        igEndCombo();
    }
    igSetItemTooltip("%s", TR("The Outliner shows only this level's actors (pick a level in Window > Levels, or All Levels)."));
}

// --- Undo (2026-11-17, Q3: a removed sublevel comes back with Ctrl+Z) ---
// Each undo step (editor.c, a copy of the world) keeps the list as it was: the world's actors and their level marks come
// back with it, so the two always agree. Only kept when the level has sublevels (NULL: none).
void* levels_undo_take(void) {
    if (!g_set.n) return NULL;
    SublevelSet* c = malloc(sizeof *c); // (an edit, not a frame)
    if (c) *c = g_set;
    return c;
}
void levels_undo_free(void* p) { free(p); }
static bool same_lighting(const SublevelSet* a, const SublevelSet* b) {
    if (a->n != b->n) return false;
    for (uint32_t i = 0; i < a->n; i++)
        if (a->s[i].lighting != b->s[i].lighting || a->s[i].id != b->s[i].id || strcmp(a->s[i].name, b->s[i].name)) return false;
    return true;
}
void levels_undo_apply(Editor* ed, const void* p) {
    static SublevelSet before; // (27 KB: not on the stack)
    before = g_set;
    if (p) g_set = *(const SublevelSet*)p;
    else memset(&g_set, 0, sizeof g_set), g_set.current = -1;
    if (!before.n && !g_set.n) return;
    if (g_view >= (int)g_set.n) set_view(LEVELS_ALL);
    g_view_rev++;
    g_confirm_remove = -1;
    ed->dirty = true;
    if (!same_lighting(&before, &g_set)) show_lighting(ed);
    else {
        for (uint32_t i = 0; i < g_set.n; i++) g_set.s[i].layer = before.s[i].layer; // (the same layers on the GPU)
        sublevels_apply_all(&ed->world, &g_set);
    }
}

bool levels_locked(Editor* ed, Entity e) {
    if (!g_set.n || ed->playing) return false;
    int s = sublevel_of(&ed->world, &g_set, e);
    return s >= 0 && g_set.s[s].locked;
}

// The save: the persistent level without the sublevels' actors, each changed sublevel to its file, the list beside it
bool levels_save(Editor* ed, const char* path, size_t* bytes) {
    if (!g_set.n) {
        sublevels_write(path, &g_set); // (a list left from before: gone)
        return level_save(&ed->world, &ed->cam, path, bytes);
    }
    bool ok = level_save_without_sublevels(&ed->world, &ed->cam, path, bytes);
    for (uint32_t i = 0; i < g_set.n && ok; i++) {
        Sublevel* s = &g_set.s[i];
        if (!s->id || !s->dirty) continue;
        if (sublevel_save(&ed->world, s)) s->dirty = false;
        else ok = false;
    }
    return ok && sublevels_write(path, &g_set);
}

// --- Build Lighting: one bake per group of lights, saved each to its file, then summed ---
static struct {
    bool on;
    int stage;  // -1 the persistent level's lights (and the non-lighting sublevels'), else a lighting level's slot
    int quality;
    LightMask mask;
    bool was_visible[SUBLEVEL_MAX];
    int done;
} g_bake;

static int next_stage(int after) {
    for (int i = after + 1; i < (int)g_set.n; i++)
        if (g_set.s[i].lighting && g_set.s[i].id) return i;
    return INT32_MAX;
}
static bool bake_stage(Editor* ed, double now) {
    sublevels_lights_only(&ed->world, &g_set, g_bake.stage, &g_bake.mask);
    gi_bake_set_no_emissive(g_bake.stage >= 0); // (the base has the emissive: once in the sum)
    if (editor_bake_start(ed, g_bake.quality, now)) return true;
    sublevels_lights_restore(&ed->world, &g_bake.mask);
    return false;
}
static void bake_finish(Editor* ed) {
    sublevels_lights_restore(&ed->world, &g_bake.mask);
    g_bake.mask.n = 0;
    gi_bake_set_no_emissive(false);
    for (uint32_t i = 0; i < g_set.n; i++)
        if (!g_bake.was_visible[i]) sublevel_set_visible(&ed->world, &g_set.s[i], false, false);
    g_bake.on = false;
    sublevels_apply_all(&ed->world, &g_set);
}

bool levels_build_lighting(Editor* ed, int quality, double now) {
    if (!any_lighting()) return false;
    if (!ed->level_path[0]) {
        editor_status(ed, now, TR("Save the level first: each Lighting Level's lighting is saved beside its own level."));
        return true;
    }
    memset(&g_bake, 0, sizeof g_bake);
    g_bake.on = true, g_bake.quality = quality, g_bake.stage = -1;
    sublevels_bake_weights(&ed->world, &g_set); // (each level baked as it is, at 1: its blend comes on top afterwards)
    for (uint32_t i = 0; i < g_set.n; i++) { // (every level shown while it bakes: the hidden ones still block and light)
        g_bake.was_visible[i] = g_set.s[i].visible;
        if (g_set.s[i].id && !g_set.s[i].visible) sublevel_set_visible(&ed->world, &g_set.s[i], true, false);
    }
    if (!bake_stage(ed, now)) bake_finish(ed);
    else editor_status(ed, now, TR("Building lighting: the persistent level, then each Lighting Level alone..."));
    return true;
}

bool levels_bake_finished(Editor* ed, double now) {
    if (!g_bake.on) return false;
    char p[1100];
    const char* owner = g_bake.stage < 0 ? ed->level_path : g_set.s[g_bake.stage].path;
    level_rlight_path(owner, p, sizeof p);
    bool saved = gi_lightmaps_save(ed->vk, p);
    if (!saved) editor_status(ed, now, TR("Lighting built, but %s could not be written."), p);
    g_bake.done += saved;
    sublevels_lights_restore(&ed->world, &g_bake.mask);
    g_bake.mask.n = 0;
    int next = next_stage(g_bake.stage);
    if (next != INT32_MAX) {
        g_bake.stage = next;
        if (bake_stage(ed, now)) {
            editor_status(ed, now, TR("Building lighting: Lighting Level %s..."), g_set.s[next].name);
            return true;
        }
    }
    int done = g_bake.done;
    bake_finish(ed);
    show_lighting(ed);
    editor_status(ed, now, TR("Lighting built: %d bakes saved; the visible Lighting Levels are added together."), done);
    return true;
}

// a cancelled bake: the lights put back
static void bake_watch(Editor* ed) {
    if (g_bake.on && !gi_baking()) bake_finish(ed);
}

// --- Play ---
void levels_play_begin(Editor* ed) {
    g_play_set = g_set;
    sublevels_begin_game(ed->vk, &ed->world, ed->level_path, &g_play_set);
}
void levels_play_open(Editor* ed, const char* path) { // Open Level while playing
    sublevels_read(path, &g_play_set);
    sublevels_begin_game(ed->vk, &ed->world, path, &g_play_set);
}
void levels_play_end(Editor* ed) {
    sublevels_set_active(&g_set, ed->level_path);
    if (g_set.n) show_lighting(ed);
    known_sync(&ed->world);
}

// --- The panel ---

typedef struct LevelList { char names[256][96]; int n; } LevelList;
static void list_one(void* user, const ReDirEntry* e);
typedef struct ListWalk { LevelList* l; char dir[900]; int depth; } ListWalk;
static void list_one(void* user, const ReDirEntry* e) {
    ListWalk* w = user;
    if (e->name[0] == '.') return;
    if (e->is_dir) {
        if (w->depth >= 10) return;
        ListWalk sub = { w->l, "", w->depth + 1 };
        snprintf(sub.dir, sizeof sub.dir, "%s\\%s", w->dir, e->name);
        re_dir_walk(sub.dir, list_one, &sub);
        return;
    }
    const char* dot = strrchr(e->name, '.');
    if (!dot || _stricmp(dot, ".rlevel") || w->l->n >= 256) return;
    snprintf(w->l->names[w->l->n], sizeof w->l->names[0], "%.*s", (int)(dot - e->name), e->name);
    w->l->n++;
}

static void add_sublevel(Editor* ed, const char* name, double now) {
    if (g_set.n >= SUBLEVEL_MAX) {
        editor_status(ed, now, TR("At most %d sublevels."), SUBLEVEL_MAX);
        return;
    }
    for (uint32_t i = 0; i < g_set.n; i++)
        if (!_stricmp(g_set.s[i].name, name)) {
            editor_status(ed, now, TR("%s is already a sublevel of this level."), name);
            return;
        }
    Sublevel* s = &g_set.s[g_set.n];
    memset(s, 0, sizeof *s);
    snprintf(s->name, sizeof s->name, "%s", name);
    s->visible = s->initially_loaded = true;
    s->weight = 1.0f, s->layer = -1;
    static const float pal[8][3] = { { 0.95f, 0.55f, 0.2f }, { 0.3f, 0.75f, 0.95f }, { 0.55f, 0.9f, 0.35f }, { 0.9f, 0.4f, 0.75f },
                                     { 0.95f, 0.85f, 0.3f }, { 0.5f, 0.5f, 0.95f }, { 0.35f, 0.9f, 0.8f }, { 0.9f, 0.35f, 0.35f } };
    memcpy(s->color, pal[g_set.n & 7u], sizeof s->color);
    g_set.n++;
    if (!sublevel_load(&ed->world, &g_set, g_set.n - 1)) {
        g_set.n--;
        editor_status(ed, now, TR("Could not load %s."), name);
        return;
    }
    bp_components_rebuild(&ed->world, NULL);
    editor_undo_clear(ed); // (Unreal too: a level coming in or out isn't undone)
    known_sync(&ed->world);
    ed->dirty = true;
    editor_status(ed, now, TR("Sublevel %s added (%zu actors)."), name, sublevels_count_actors(&ed->world, s));
}

// the levels' marks renumbered (SUBLEVEL_ID0 + slot) after the slots moved: map[old id] = new id (0: unchanged). One
// pass: each mark read once, so a swap (2 -> 3, 3 -> 2) is right
static void renumber(Editor* ed, const uint32_t* map) {
    for (uint32_t ai = 0; ai < ed->world.archetype_count; ai++) {
        const EcsArchetype* a = &ed->world.archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_STREAMED))) continue;
        CStreamed* st = ecs_arch_column(a, COMP_STREAMED);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t k = 0; k < a->count; k++)
            if (st[k].level < SUBLEVEL_ID0 + SUBLEVEL_MAX && map[st[k].level]) {
                st[k].level = map[st[k].level];
                ecs_touch(&ed->world, ents[k]);
            }
    }
}

static void remove_sublevel(Editor* ed, uint32_t slot, double now) {
    if (slot >= g_set.n) return;
    if (g_set.s[slot].dirty && g_set.s[slot].id) sublevel_save(&ed->world, &g_set.s[slot]);
    editor_undo_push(ed); // (2026-11-17: Ctrl+Z brings it back, its actors and its place in the list)
    char name[96];
    snprintf(name, sizeof name, "%s", g_set.s[slot].name);
    sublevel_unload(&ed->world, &g_set, slot);
    // the slots after it move down: their actors' marks with them (SUBLEVEL_ID0 + slot)
    uint32_t map[SUBLEVEL_ID0 + SUBLEVEL_MAX] = { 0 };
    for (uint32_t i = slot + 1; i < g_set.n; i++) {
        Sublevel* s = &g_set.s[i];
        if (s->id) map[s->id] = SUBLEVEL_ID0 + i - 1, s->id = SUBLEVEL_ID0 + i - 1;
        g_set.s[i - 1] = *s;
    }
    renumber(ed, map);
    g_set.n--;
    if (g_set.current == (int)slot) g_set.current = -1;
    else if (g_set.current > (int)slot) g_set.current--;
    if (g_view == (int)slot) set_view(LEVELS_ALL);
    else if (g_view > (int)slot) set_view(g_view - 1);
    editor_select_none(ed);
    known_sync(&ed->world);
    ed->dirty = true;
    show_lighting(ed);
    editor_status(ed, now, TR("Sublevel %s removed (its file is kept). Ctrl+Z brings it back."), name);
}

// (2026-11-17) a sublevel dragged to another place in the list: the slots between shift, their marks with them
static void move_slot(Editor* ed, uint32_t from, uint32_t to) {
    if (from >= g_set.n || to >= g_set.n || from == to) return;
    editor_undo_push(ed);
    int where[SUBLEVEL_MAX]; // where each old slot goes
    for (uint32_t k = 0; k < g_set.n; k++) {
        int v = (int)k;
        if (k == from) v = (int)to;
        else if (from < to && k > from && k <= to) v = (int)k - 1;
        else if (from > to && k >= to && k < from) v = (int)k + 1;
        where[k] = v;
    }
    static Sublevel old[SUBLEVEL_MAX];
    memcpy(old, g_set.s, sizeof(Sublevel) * g_set.n);
    uint32_t map[SUBLEVEL_ID0 + SUBLEVEL_MAX] = { 0 };
    for (uint32_t k = 0; k < g_set.n; k++) {
        Sublevel* s = &g_set.s[where[k]];
        *s = old[k];
        if (s->id) {
            uint32_t id = SUBLEVEL_ID0 + (uint32_t)where[k];
            if (id != s->id) map[s->id] = id;
            s->id = id;
        }
    }
    renumber(ed, map);
    if (g_set.current >= 0) g_set.current = where[g_set.current];
    if (g_view >= 0) set_view(where[g_view]);
    g_view_rev++;
    known_sync(&ed->world);
    ed->dirty = true;
    show_lighting(ed);
}

static void create_sublevel(Editor* ed, const char* name, double now) {
    if (!ed->level_path[0]) {
        editor_status(ed, now, TR("Save the level first: its sublevels are saved beside it."));
        return;
    }
    char dir[1024], path[1200];
    snprintf(dir, sizeof dir, "%s", ed->level_path);
    char* sl = strrchr(dir, '\\');
    char* sl2 = strrchr(dir, '/');
    if (sl2 > sl) sl = sl2;
    if (sl) *sl = 0;
    snprintf(path, sizeof path, "%s\\%s.rlevel", dir, name);
    if (re_file_stat(path, NULL, NULL, NULL)) {
        editor_status(ed, now, TR("%s already exists: use Add Existing."), name);
        return;
    }
    static World empty;
    if (!ecs_init(&empty)) return;
    LevelCamera cam = ed->cam;
    bool ok = level_save(&empty, &cam, path, NULL);
    ecs_shutdown(&empty);
    content_mark_dirty();
    if (!ok) {
        editor_status(ed, now, TR("Could not save %s."), path);
        return;
    }
    add_sublevel(ed, name, now);
    if (g_set.n && !_stricmp(g_set.s[g_set.n - 1].name, name)) {
        snprintf(g_set.s[g_set.n - 1].path, sizeof g_set.s[0].path, "%s", path);
        g_set.current = (int)g_set.n - 1;
        known_sync(&ed->world);
    }
}

static void move_selected(Editor* ed, int slot, double now) {
    uint32_t moved = 0;
    for (uint32_t i = 0; i < ed->nsel; i++) {
        Entity e = ed->sel[i];
        if (!ecs_alive(&ed->world, e)) continue;
        int from = sublevel_of(&ed->world, &g_set, e);
        if (from == slot) continue;
        if (from >= 0) g_set.s[from].dirty = true;
        sublevel_move_actor(&ed->world, &g_set, e, slot);
        moved++;
    }
    if (slot >= 0) g_set.s[slot].dirty = true;
    if (moved) ed->dirty = true;
    const char* where = slot < 0 ? TR("the Persistent Level") : g_set.s[slot].name;
    if (moved == 1) editor_status(ed, now, TR("1 actor moved to %s."), where); // (2026-11-19: "1 actors")
    else editor_status(ed, now, TR("%u actors moved to %s."), moved, where);
}

static void select_level(Editor* ed, int slot) {
    editor_select_none(ed);
    World* w = &ed->world;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!a->count || !(a->mask & COMP_BIT(COMP_TRANSFORM)) || (a->mask & COMP_BIT(COMP_ATTACH))) continue;
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (sublevel_of(w, &g_set, ents[i]) == slot) editor_select(ed, ents[i], SEL_ADD);
    }
}

bool* levels_show_flag(void) { return &g_show; }
// (2026-11-19) the panel's toolbar: the next button beside the last when it fits, else on the next line
static void toolbar_next(const char* label) {
    float need = igCalcTextSize(label, NULL, false, -1).x + igGetFrameHeight() + igGetStyle()->FramePadding.x * 3.0f;
    float right = igGetItemRectMax().x, edge = igGetWindowPos().x + igGetWindowWidth() - igGetStyle()->WindowPadding.x;
    if (right + 6.0f + need <= edge) igSameLine(0, 6);
}
static bool g_focused; // (the panel had the focus last frame: Del is the sublevel's there)
bool levels_panel_focused(void) { return g_focused; }

static void make_current(Editor* ed, int slot) {
    g_set.current = slot;
    known_sync(&ed->world);
}

static void row_menu(Editor* ed, int slot, double now) {
    if (!igBeginPopupContextItem("lvl_row", ImGuiPopupFlags_MouseButtonRight)) return;
    if (ui_menu_item(ICON_CHECK, TR("Make Current"), NULL, g_set.current == slot, true)) make_current(ed, slot);
    if (ui_menu_item(ICON_SEARCH, TR("Show Only Its Actors in the Outliner"), NULL, g_view == slot, true)) set_view(g_view == slot ? LEVELS_ALL : slot);
    if (ui_menu_item(ICON_MOVE, TR("Move Selected Actors to Level"), NULL, false, ed->nsel > 0)) move_selected(ed, slot, now);
    if (ui_menu_item(ICON_SELECT, TR("Select Actors"), NULL, false, true)) select_level(ed, slot);
    if (slot >= 0) {
        Sublevel* s = &g_set.s[slot];
        if (ui_menu_item(ICON_SAVE, TR("Save Level"), NULL, false, s->id != 0)) {
            if (sublevel_save(&ed->world, s)) s->dirty = false, editor_status(ed, now, TR("Saved %s."), s->path);
        }
        igSeparator();
        if (ui_menu_item(ICON_DELETE, TR("Remove Selected Level"), "Del", false, true)) g_confirm_remove = slot;
    }
    igEndPopup();
}

// a small padlock (no icon of its own in ui_icons): closed or open
static void draw_lock(ImDrawList* dl, ImVec2 p, float sz, bool closed, uint32_t col) {
    float w = sz * 0.56f, h = sz * 0.4f, x0 = p.x + (sz - w) * 0.5f, y0 = p.y + sz * 0.48f;
    ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x0 + w, y0 + h }, col, sz * 0.08f, 0);
    float r = w * 0.32f, cx = p.x + sz * 0.5f + (closed ? 0.0f : w * 0.35f), cy = y0;
    ImDrawList_PathArcTo(dl, (ImVec2){ cx, cy }, r, 3.14159f, 6.28318f, 10);
    ImDrawList_PathStroke(dl, col, sz * 0.1f, 0);
}

// One level as a card: a stripe of its color, the eye, its kind (Lighting: a sun, Data: the data icon), its name and
// marks (current, changed, missing), its actors; Lighting Levels their blend. Click: the Outliner shows its actors;
// double-click: current; drag: to another place in the list; drop actors from the Outliner: moved into it.
// Returns the slot dropped onto it (another card), else -100.
static int level_card(Editor* ed, int slot, size_t actors, bool can, double now) {
    Sublevel* s = slot >= 0 ? &g_set.s[slot] : NULL;
    ImGuiStyle* st = igGetStyle();
    float fs = igGetFontSize(), fh = igGetFrameHeight(), pad = fs * 0.45f;
    ImVec2 p0 = igGetCursorScreenPos(), avail = igGetContentRegionAvail();
    float h = fh * 2.0f + pad * 1.6f, wdt = avail.x;
    bool picked = g_view == slot, current = g_set.current == slot;
    igPushID_Int(slot);
    igSetNextItemAllowOverlap();
    igInvisibleButton("card", (ImVec2){ wdt, h }, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight); // tips: none (a canvas, a node or a tile drawn with its own name and text)
    rect_rec(slot, LV_R_CARD);
    bool hovered = igIsItemHovered(0);
    if (igIsItemClicked(ImGuiMouseButton_Left)) set_view(picked ? LEVELS_ALL : slot);
    if (hovered && igIsMouseDoubleClicked_Nil(ImGuiMouseButton_Left) && can) make_current(ed, slot), set_view(slot);
    if (can) row_menu(ed, slot, now);
    int dropped = -100;
    if (s && can && igBeginDragDropSource(0)) { // reorder
        igSetDragDropPayload("RE_LEVEL_SLOT", &slot, sizeof slot, ImGuiCond_Once);
        igText(TR("Move %s in the list"), s->name);
        igEndDragDropSource();
    }
    if (can && igBeginDragDropTarget()) {
        const ImGuiPayload* pl = igAcceptDragDropPayload("RE_LEVEL_SLOT", 0);
        if (pl) dropped = *(const int*)pl->Data;
        if (igAcceptDragDropPayload("RE_OL_ACTORS", 0)) move_selected(ed, slot, now);
        igEndDragDropTarget();
    }
    ImDrawList* dl = igGetWindowDrawList();
    ImVec2 p1 = { p0.x + wdt, p0.y + h };
    uint32_t bg = igGetColorU32_Col(picked ? ImGuiCol_Header : hovered ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg, 1.0f);
    ImDrawList_AddRectFilled(dl, p0, p1, bg, st->FrameRounding + 3.0f, 0);
    if (current) ImDrawList_AddRect(dl, p0, p1, igGetColorU32_Col(ImGuiCol_CheckMark, 0.9f), st->FrameRounding + 3.0f, 1.5f, 0);
    float c3[3] = { 0.55f, 0.55f, 0.6f };
    if (s) memcpy(c3, s->color, sizeof c3);
    uint32_t lc = igColorConvertFloat4ToU32((ImVec4){ c3[0], c3[1], c3[2], 1.0f });
    ImDrawList_AddRectFilled(dl, p0, (ImVec2){ p0.x + fs * 0.3f, p1.y }, lc, st->FrameRounding + 3.0f, ImDrawFlags_RoundCornersLeft);
    uint32_t tc = igGetColorU32_Col(ImGuiCol_Text, 1.0f), dc = igGetColorU32_Col(ImGuiCol_TextDisabled, 1.0f);

    // first line: eye, kind, name, marks ... actors
    float x = p0.x + fs * 0.3f + pad, y = p0.y + pad * 0.8f;
    igSetCursorScreenPos((ImVec2){ x, y });
    if (s) {
        bool vis = s->visible;
        if (ui_icon_button("eye", vis ? ICON_EYE : ICON_EYE_CLOSED, NULL, vis ? tc : dc, false, TR("Show or hide this level in the editor")) && can) {
            vis = !vis;
            sublevel_set_visible(&ed->world, s, vis, false);
            if (s->lighting && s->layer >= 0) sublevels_apply_weight(&ed->world, s); // (its layer: the GPU blend, no reload)
            else if (s->lighting) show_lighting(ed);
        }
        rect_rec(slot, LV_R_EYE);
    } else {
        ui_icon_item(ICON_LEVEL, fh, tc);
    }
    igSameLine(0, 4);
    if (s) {
        UiIcon kind = s->lighting ? ICON_SUN : ICON_DATA;
        if (ui_icon_button("kind", kind, NULL, s->lighting ? TH(TK_AMBER) : dc, s->lighting,
                           s->lighting ? TR("Lighting Level: its lights, sky, clouds, fog and Post Process bake and blend on their own. Click: a Data level")
                                       : TR("Data level: actors only. Click: a Lighting Level (its lighting baked on its own and added)")) && can) {
            s->lighting = !s->lighting, ed->dirty = true;
            show_lighting(ed);
        }
        rect_rec(slot, LV_R_KIND);
        igSameLine(0, 6);
    }
    char label[200];
    snprintf(label, sizeof label, "%s%s%s", s ? s->name : TR("Persistent Level"), s && s->dirty ? " *" : "", s && !s->id ? TR(" (missing)") : "");
    igAlignTextToFramePadding();
    igTextUnformatted(label, NULL);
    if (current) {
        igSameLine(0, 8);
        igTextColored(st->Colors[ImGuiCol_CheckMark], "%s", TR("current"));
    }
    char cnt[48];
    if (actors == 1) snprintf(cnt, sizeof cnt, "%s", TR("1 actor"));
    else snprintf(cnt, sizeof cnt, TR("%zu actors"), actors);
    float cw = igCalcTextSize(cnt, NULL, false, -1).x;
    ImDrawList_AddText_Vec2(dl, (ImVec2){ p1.x - cw - pad, y + (fh - fs) * 0.5f }, dc, cnt, NULL);

    // second line: the Lighting Level's blend, initially loaded, lock, remove
    igSetCursorScreenPos((ImVec2){ x, y + fh + pad * 0.3f });
    if (s) {
        if (s->lighting) {
            igAlignTextToFramePadding();
            igTextDisabled("%s", TR("Blend")); // (2026-11-19: inside the slider the grab covered the word)
            igSameLine(0, 4);
            igSetNextItemWidth(fs * 4.5f);
            if (ui_num_float("##lvlblend", &s->weight, 0.01f, 0.0f, 2.0f, 0.0f, 3.4e38f, "%.2f", 0)) { // (a drag: a slider's grab covered the number)
                ed->dirty = true;
                sublevels_apply_weight(&ed->world, s);
            }
            rect_rec(slot, LV_R_BLEND);
            igSetItemTooltip("%s", TR("Its share of the light: 0 off, 1 as baked, more brighter (Set Lighting Level Blend)"));
            igSameLine(0, 10);
        }
        bool init = s->initially_loaded;
        if (igCheckbox(TR("At start"), &init) && can) s->initially_loaded = init, ed->dirty = true;
        rect_rec(slot, LV_R_START);
        igSetItemTooltip("%s", TR("Initially Loaded: the game and the Play start with it in"));
        igSameLine(0, 10);
        ImVec2 lp = igGetCursorScreenPos();
        if (igInvisibleButton("lock", (ImVec2){ fh, fh }, 0) && can) {
            s->locked = !s->locked;
            if (s->locked) editor_select_none(ed);
        }
        rect_rec(slot, LV_R_LOCK);
        igSetItemTooltip("%s", s->locked ? TR("Locked: its actors aren't selected or moved. Click to unlock") : TR("Lock: its actors can't be selected or moved"));
        draw_lock(dl, lp, fh, s->locked, s->locked ? TH(TK_ORANGE) : dc);
        igSameLine(0, 4);
        igColorEdit3("##col", s->color, ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
        igSetItemTooltip("%s", TR("The level's color (its card, the Outliner's header)"));
        float bw = fh + st->FramePadding.x * 2.0f;
        igSameLine(0, 0);
        igSetCursorScreenPos((ImVec2){ p1.x - bw - pad, igGetCursorScreenPos().y });
        if (ui_icon_button("rm", ICON_DELETE, NULL, TH(TK_RED), false, TR("Remove this sublevel from the level (asks first; its file is kept; Ctrl+Z brings it back)")) && can)
            g_confirm_remove = slot;
        rect_rec(slot, LV_R_TRASH);
    } else {
        igTextDisabled("%s", TR("The level you opened: what isn't in a sublevel."));
    }
    igSetCursorScreenPos((ImVec2){ p0.x, p1.y + pad * 0.7f });
    igDummy((ImVec2){ 0, 0 });
    igPopID();
    return dropped;
}

static void confirm_remove_dialog(Editor* ed, double now) {
    if (g_confirm_remove >= 0 && g_confirm_remove < (int)g_set.n && !igIsPopupOpen_Str("###lvl_remove", 0)) igOpenPopup_Str("###lvl_remove", 0);
    char title[160];
    snprintf(title, sizeof title, "%s###lvl_remove", TR("Remove Sublevel"));
    if (!igBeginPopupModal(title, NULL, ImGuiWindowFlags_AlwaysAutoResize)) return;
    if (g_confirm_remove < 0 || g_confirm_remove >= (int)g_set.n) {
        igCloseCurrentPopup();
        igEndPopup();
        return;
    }
    Sublevel* s = &g_set.s[g_confirm_remove];
    igPushTextWrapPos(igGetFontSize() * 30.0f); // (2026-11-19: one long line made it as wide as the screen)
    igText(TR("Remove %s from this level?"), s->name);
    igTextDisabled(TR("Its %zu actors leave the world; its file %s.rlevel stays in the project (Add Existing brings it back)."),
                   sublevels_count_actors(&ed->world, s), s->name);
    igTextDisabled("%s", TR("Ctrl+Z undoes it."));
    igPopTextWrapPos();
    igSpacing();
    if (igButton(TR("Remove"), (ImVec2){ igGetFontSize() * 7, 0 }) || igIsKeyPressed_Bool(ImGuiKey_Enter, false)) {
        remove_sublevel(ed, (uint32_t)g_confirm_remove, now);
        g_confirm_remove = -1;
        igCloseCurrentPopup();
    }
    igSameLine(0, 8);
    if (igButton(TR("Cancel"), (ImVec2){ igGetFontSize() * 7, 0 }) || igIsKeyPressed_Bool(ImGuiKey_Escape, false)) {
        g_confirm_remove = -1;
        igCloseCurrentPopup();
    }
    igEndPopup();
}

void levels_frame(Editor* ed, double now) {
    bake_watch(ed);
    if (!ed->playing) assign_new_actors(ed);
    { // (2026-11-16, crashes #135-137) REALENGINE_ADD_SUBLEVEL=name: added as by the panel 5 s after the start, again every
      // 3 s (removed, added back): the test of what the panel's Add did to the frame
        static const char* want = (const char*)1;
        static double next = 0.0;
        if (want == (const char*)1) want = getenv("REALENGINE_ADD_SUBLEVEL");
        if (want && !ed->playing) {
            if (next == 0.0) next = now + 5.0;
            if (now >= next) {
                next = now + 3.0;
                int at = -1;
                for (uint32_t i = 0; i < g_set.n; i++)
                    if (!_stricmp(g_set.s[i].name, want)) at = (int)i;
                if (at >= 0) remove_sublevel(ed, (uint32_t)at, now);
                else add_sublevel(ed, want, now);
                printf("[RealEngine] test: sublevel %s %s\n", want, at >= 0 ? "removed" : "added");
            }
        }
    }
    g_focused = false;
    if (levels_test_hook) levels_test_hook(ed, now);
    if (!g_show) return;
    igSetNextWindowSize((ImVec2){ igGetFontSize() * 30, igGetFontSize() * 26 }, ImGuiCond_FirstUseEver);
    char lv_title[96]; // (a stable name: Q2's layouts dock it beside the Outliner, editor_dock.c)
    snprintf(lv_title, sizeof lv_title, "%s###re_levels", TR("Levels"));
    if (igBegin(lv_title, &g_show, 0)) {
        g_focused = igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
        bool can = !ed->playing && !g_bake.on;
        igBeginDisabled(!can);
        static LevelList list;
        if (ui_icon_button("lvl_addb", ICON_PLUS, TR("Add Existing..."), TH(TK_LIME), false, TR("Add a level of the project as a sublevel"))) {
            list.n = 0;
            ListWalk w = { &list, "", 0 };
            snprintf(w.dir, sizeof w.dir, "%s", content_root());
            re_dir_walk(w.dir, list_one, &w);
            igOpenPopup_Str("lvl_add", 0);
        }
        if (igBeginPopup("lvl_add", 0)) {
            char me[128] = "";
            const char* b = strrchr(ed->level_path, '\\');
            const char* b2 = strrchr(ed->level_path, '/');
            if (b2 > b) b = b2;
            snprintf(me, sizeof me, "%s", b ? b + 1 : ed->level_path);
            char* dot = strrchr(me, '.');
            if (dot) *dot = 0;
            int shown = 0;
            for (int i = 0; i < list.n; i++) {
                if (!_stricmp(list.names[i], me)) continue;
                if (ui_menu_item(ICON_LEVEL, list.names[i], NULL, false, true)) add_sublevel(ed, list.names[i], now);
                shown++;
            }
            if (!shown) igTextDisabled("%s", TR("No other level in the project."));
            igEndPopup();
        }
        toolbar_next(TR("Create New..."));
        static char new_name[96] = "Lighting_Night";
        static bool new_lighting = true;
        if (ui_icon_button("lvl_newb", ICON_NEW, TR("Create New..."), TH(TK_N14), false, TR("A new empty level beside this one, made current"))) igOpenPopup_Str("lvl_new", 0);
        if (igBeginPopup("lvl_new", 0)) {
            igSetNextItemWidth(igGetFontSize() * 14);
            igInputText("##lvlname", new_name, sizeof new_name, 0, NULL, NULL);
            igCheckbox(TR("Lighting Level"), &new_lighting);
            if (igButton(TR("Create"), (ImVec2){ 0, 0 }) && new_name[0]) {
                uint32_t n0 = g_set.n;
                create_sublevel(ed, new_name, now);
                if (g_set.n == n0 + 1 && new_lighting) g_set.s[n0].lighting = true, show_lighting(ed);
                igCloseCurrentPopup();
            }
            igEndPopup();
        }
        toolbar_next(TR("Save Levels"));
        if (ui_icon_button("lvl_saveb", ICON_SAVE, TR("Save Levels"), TH(TK_N14), false, TR("Save every changed sublevel and the list"))) {
            int saved = 0;
            for (uint32_t i = 0; i < g_set.n; i++)
                if (g_set.s[i].id && g_set.s[i].dirty && sublevel_save(&ed->world, &g_set.s[i])) g_set.s[i].dirty = false, saved++;
            if (ed->level_path[0]) sublevels_write(ed->level_path, &g_set);
            editor_status(ed, now, TR("%d sublevels saved."), saved);
        }
        toolbar_next(TR("Remove"));
        int target = g_view >= 0 ? g_view : g_set.current;
        igBeginDisabled(target < 0);
        if (ui_icon_button("lvl_rmb", ICON_DELETE, TR("Remove"), TH(TK_RED), false, TR("Remove the selected sublevel (Del; asks first; Ctrl+Z brings it back)")))
            g_confirm_remove = target;
        igEndDisabled();
        igEndDisabled();

        igSeparator();
        int lit_on = 0;
        static size_t counts[SUBLEVEL_MAX + 1]; // (the actors counted twice a second, not each frame; [n]: the persistent's)
        static double counted_at = -1.0;
        if (now - counted_at > 0.5 || now < counted_at) {
            size_t total = 0;
            World* w = &ed->world;
            for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
                const EcsArchetype* a = &w->archetypes[ai];
                if (a->count && (a->mask & COMP_BIT(COMP_TRANSFORM)) && !(a->mask & COMP_BIT(COMP_ATTACH))) total += a->count;
            }
            size_t in_subs = 0;
            for (uint32_t i = 0; i < g_set.n; i++) counts[i] = sublevels_count_actors(&ed->world, &g_set.s[i]), in_subs += counts[i];
            counts[SUBLEVEL_MAX] = total > in_subs ? total - in_subs : 0;
            counted_at = now;
        }
        char foot1[400], foot2[300] = "";
        snprintf(foot1, sizeof foot1, TR("Current level (new actors go there): %s. Click a level: the Outliner shows its actors; double-click: current; drag: reorder."),
                 g_set.current < 0 ? TR("Persistent Level") : g_set.s[g_set.current].name);
        int lit_vis = 0;
        for (uint32_t i = 0; i < g_set.n; i++) lit_vis += g_set.s[i].lighting && g_set.s[i].visible;
        if (any_lighting())
            snprintf(foot2, sizeof foot2, TR("Lighting Levels on: %d, added over the persistent level's lighting (any mix at once). Build Lighting bakes each one."), lit_vis);
        float ww = igGetContentRegionAvail().x, foot_h = igCalcTextSize(foot1, NULL, false, ww).y + (foot2[0] ? igCalcTextSize(foot2, NULL, false, ww).y : 0.0f);
        igBeginChild_Str("lvl_cards", (ImVec2){ 0, -(foot_h + igGetStyle()->ItemSpacing.y * 3.0f) }, 0, 0);
        level_card(ed, -1, counts[SUBLEVEL_MAX], can, now);
        int drag_from = -1, drag_to = -1;
        for (uint32_t i = 0; i < g_set.n; i++) {
            int d = level_card(ed, (int)i, counts[i], can, now);
            if (d >= 0) drag_from = d, drag_to = (int)i;
            lit_on += g_set.s[i].lighting && g_set.s[i].visible;
        }
        igEndChild();
        if (drag_from >= 0 && drag_to >= 0) move_slot(ed, (uint32_t)drag_from, (uint32_t)drag_to);
        // Del in the panel: the picked sublevel (asks first)
        if (can && g_view >= 0 && g_confirm_remove < 0 && igIsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) && !igIsAnyItemActive() &&
            igIsKeyPressed_Bool(ImGuiKey_Delete, false))
            g_confirm_remove = g_view;
        confirm_remove_dialog(ed, now);
        (void)lit_on;
        igPushStyleColor_U32(ImGuiCol_Text, igGetColorU32_Col(ImGuiCol_TextDisabled, 1.0f));
        igPushTextWrapPos(0.0f);
        igTextUnformatted(foot1, NULL);
        if (foot2[0]) igTextUnformatted(foot2, NULL);
        igPopTextWrapPos();
        igPopStyleColor(1);
    }
    igEnd();
}

// --- The test's hooks (levels_test.c) ---
SublevelSet* levels_set(void) { return &g_set; }
bool levels_create(Editor* ed, const char* name, double now) {
    uint32_t n0 = g_set.n;
    create_sublevel(ed, name, now);
    return g_set.n == n0 + 1;
}
void levels_assign_now(Editor* ed) { assign_new_actors(ed); }
void levels_set_current(Editor* ed, int slot) {
    g_set.current = slot;
    known_sync(&ed->world);
}
void levels_move_selected(Editor* ed, int slot, double now) { move_selected(ed, slot, now); }
bool levels_baking(void) { return g_bake.on; }
void levels_remove(Editor* ed, int slot, double now) { remove_sublevel(ed, (uint32_t)slot, now); }
void levels_move_slot(Editor* ed, int from, int to) { move_slot(ed, (uint32_t)from, (uint32_t)to); }
