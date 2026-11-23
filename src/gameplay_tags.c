// Gameplay Tags (gameplay_tags.h)
#include "gameplay_tags.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "components.h"

static int lower(int c) { return c >= 'A' && c <= 'Z' ? c + 32 : c; }

bool gt_matches(const char* tag, const char* query, bool exact) {
    if (!tag || !query || !query[0]) return false;
    size_t i = 0;
    for (; query[i]; i++)
        if (lower((unsigned char)tag[i]) != lower((unsigned char)query[i])) return false;
    return tag[i] == 0 || (!exact && tag[i] == '.');
}

bool gt_clean(const char* in, char* out, size_t cap) {
    size_t n = 0;
    bool dot = true; // (no leading dot)
    for (const char* p = in ? in : ""; *p && n + 1 < cap; p++) {
        unsigned char c = (unsigned char)*p;
        if (isspace(c)) continue;
        if (c == '.') {
            if (dot) continue;
            dot = true;
        } else
            dot = false;
        out[n++] = (char)c;
    }
    while (n && out[n - 1] == '.') n--;
    if (cap) out[n] = 0;
    return n > 0;
}

bool gt_has(World* w, Entity e, const char* query, bool exact) {
    const CGameplayTags* g = w && ecs_alive(w, e) ? ecs_get(w, e, COMP_GAMEPLAY_TAGS) : NULL;
    for (uint32_t i = 0; g && i < g->count && i < GT_MAX_TAGS; i++)
        if (gt_matches(g->tags[i], query, exact)) return true;
    return false;
}

bool gt_add(World* w, Entity e, const char* tag) {
    char t[GT_TAG_LEN];
    if (!w || !ecs_alive(w, e) || !gt_clean(tag, t, sizeof t)) return false;
    if (!ecs_get(w, e, COMP_GAMEPLAY_TAGS) && !ecs_add(w, e, COMP_GAMEPLAY_TAGS)) return false;
    CGameplayTags* g = ecs_get(w, e, COMP_GAMEPLAY_TAGS);
    for (uint32_t i = 0; i < g->count; i++)
        if (gt_matches(g->tags[i], t, true)) return true;
    if (g->count >= GT_MAX_TAGS) return false;
    snprintf(g->tags[g->count++], GT_TAG_LEN, "%s", t);
    return true;
}

bool gt_remove(World* w, Entity e, const char* tag) {
    char t[GT_TAG_LEN];
    CGameplayTags* g = w && ecs_alive(w, e) ? ecs_get(w, e, COMP_GAMEPLAY_TAGS) : NULL;
    if (!g || !gt_clean(tag, t, sizeof t)) return false;
    for (uint32_t i = 0; i < g->count; i++) {
        if (!gt_matches(g->tags[i], t, true)) continue;
        memmove(g->tags[i], g->tags[i + 1], (size_t)(g->count - i - 1) * GT_TAG_LEN);
        memset(g->tags[--g->count], 0, GT_TAG_LEN);
        return true;
    }
    return false;
}

// --- The project's tag list (Project Settings > Gameplay Tags): Config/DefaultGameplayTags.ini as Unreal writes it,
// one "+GameplayTagList=(Tag="A.B",DevComment="")" a line. Read when the project changes (project_file), kept sorted ---

#include "platform.h"
#include "platform_files.h"
#include "project.h"

static char g_list[GT_PROJECT_MAX][GT_TAG_LEN];
static uint32_t g_nlist;
static char g_list_for[600]; // the project file the list was read for
static bool g_list_read;

static int tag_cmp(const void* a, const void* b) { return _stricmp((const char*)a, (const char*)b); }

static void list_path(char* out, size_t cap) {
    char root[600];
    project_root(root, sizeof root);
    snprintf(out, cap, "%s/Config/DefaultGameplayTags.ini", root);
}

static void list_sync(void) {
    const char* pf = project_file();
    if (g_list_read && !strcmp(pf, g_list_for)) return;
    g_list_read = true;
    snprintf(g_list_for, sizeof g_list_for, "%s", pf);
    g_nlist = 0;
    if (!pf[0]) return; // (no project: the list lives until the editor closes)
    char path[700];
    list_path(path, sizeof path);
    FILE* f = re_fopen(path, "rb");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f) && g_nlist < GT_PROJECT_MAX) {
        const char* t = strstr(line, "Tag=\"");
        if (!t || strncmp(line, "+GameplayTagList", 16)) continue;
        t += 5;
        const char* q = strchr(t, '"');
        if (!q) continue;
        char raw[GT_TAG_LEN * 2], clean[GT_TAG_LEN];
        size_t n = (size_t)(q - t) < sizeof raw - 1 ? (size_t)(q - t) : sizeof raw - 1;
        memcpy(raw, t, n), raw[n] = 0;
        if (gt_clean(raw, clean, sizeof clean) && gt_project_index(clean) < 0) snprintf(g_list[g_nlist++], GT_TAG_LEN, "%s", clean);
    }
    fclose(f);
    qsort(g_list, g_nlist, GT_TAG_LEN, tag_cmp);
}

static bool list_save(void) {
    qsort(g_list, g_nlist, GT_TAG_LEN, tag_cmp);
    if (!project_file()[0]) return true;
    char path[700], dir[700];
    list_path(path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    char* s = strrchr(dir, '/');
    if (s) *s = 0;
    re_make_dirs(dir);
    FILE* f = re_fopen(path, "wb");
    if (!f) return false;
    fputs("[/Script/GameplayTags.GameplayTagsSettings]\r\nImportTagsFromConfig=True\r\n", f);
    for (uint32_t i = 0; i < g_nlist; i++) fprintf(f, "+GameplayTagList=(Tag=\"%s\",DevComment=\"\")\r\n", g_list[i]);
    fclose(f);
    return true;
}

uint32_t gt_project_count(void) {
    list_sync();
    return g_nlist;
}
const char* gt_project_at(uint32_t i) {
    list_sync();
    return i < g_nlist ? g_list[i] : "";
}
int gt_project_index(const char* tag) {
    for (uint32_t i = 0; i < g_nlist; i++)
        if (!_stricmp(g_list[i], tag)) return (int)i;
    return -1;
}

bool gt_project_add(const char* tag) {
    list_sync();
    char t[GT_TAG_LEN];
    if (!gt_clean(tag, t, sizeof t)) return false;
    if (gt_project_index(t) >= 0) return true;
    if (g_nlist >= GT_PROJECT_MAX) return false;
    snprintf(g_list[g_nlist++], GT_TAG_LEN, "%s", t);
    return list_save();
}

// A tag renamed with what is under it ("Damage" -> "Hit": "Damage.Fire" -> "Hit.Fire"); n: how many
static uint32_t rename_under(char (*tags)[GT_TAG_LEN], uint32_t count, const char* from, const char* to) {
    uint32_t n = 0;
    size_t fl = strlen(from);
    for (uint32_t i = 0; i < count; i++) {
        if (!gt_matches(tags[i], from, false)) continue;
        char nt[GT_TAG_LEN * 2];
        snprintf(nt, sizeof nt, "%s%s", to, tags[i] + fl);
        if (strlen(nt) >= GT_TAG_LEN) continue; // (too long: kept)
        snprintf(tags[i], GT_TAG_LEN, "%s", nt);
        n++;
    }
    return n;
}

uint32_t gt_project_rename(World* w, const char* from, const char* to) {
    list_sync();
    char f[GT_TAG_LEN], t[GT_TAG_LEN];
    if (!gt_clean(from, f, sizeof f) || !gt_clean(to, t, sizeof t) || !_stricmp(f, t)) return 0;
    uint32_t n = rename_under(g_list, g_nlist, f, t);
    // (a renamed tag landing on one the list has: once)
    for (uint32_t i = 0; i < g_nlist; i++)
        for (uint32_t j = i + 1; j < g_nlist;)
            if (!_stricmp(g_list[i], g_list[j])) memmove(g_list[j], g_list[j + 1], (size_t)(--g_nlist - j) * GT_TAG_LEN);
            else j++;
    list_save();
    // the level's actors that have them
    for (uint32_t ai = 0; w && ai < w->archetype_count; ai++) {
        EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_GAMEPLAY_TAGS)) || !a->count) continue;
        CGameplayTags* g = ecs_arch_column(a, COMP_GAMEPLAY_TAGS);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            if (rename_under(g[i].tags, g[i].count < GT_MAX_TAGS ? g[i].count : GT_MAX_TAGS, f, t)) n++, ecs_touch(w, ents[i]);
    }
    return n;
}

uint32_t gt_project_remove(const char* tag) {
    list_sync();
    char t[GT_TAG_LEN];
    if (!gt_clean(tag, t, sizeof t)) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_nlist;)
        if (gt_matches(g_list[i], t, false)) memmove(g_list[i], g_list[i + 1], (size_t)(--g_nlist - i) * GT_TAG_LEN), n++;
        else i++;
    if (n) list_save();
    return n;
}

uint32_t gt_project_import_level(World* w) {
    list_sync();
    uint32_t n = 0;
    for (uint32_t ai = 0; w && ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_GAMEPLAY_TAGS)) || !a->count) continue;
        const CGameplayTags* g = ecs_arch_column(a, COMP_GAMEPLAY_TAGS);
        for (uint32_t i = 0; i < a->count; i++)
            for (uint32_t k = 0; k < g[i].count && k < GT_MAX_TAGS && g_nlist < GT_PROJECT_MAX; k++)
                if (gt_project_index(g[i].tags[k]) < 0) snprintf(g_list[g_nlist++], GT_TAG_LEN, "%s", g[i].tags[k]), n++;
    }
    if (n) list_save();
    return n;
}

uint32_t gt_find_actors(World* w, const char* query, bool exact, Entity* out, uint32_t cap) {
    uint32_t n = 0;
    if (!w || !query || !query[0]) return 0;
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_GAMEPLAY_TAGS)) || !a->count || (a->mask & COMP_BIT(COMP_ATTACH))) continue;
        const CGameplayTags* g = ecs_arch_column(a, COMP_GAMEPLAY_TAGS);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++)
            for (uint32_t k = 0; k < g[i].count && k < GT_MAX_TAGS; k++)
                if (gt_matches(g[i].tags[k], query, exact)) {
                    if (out && n < cap) out[n] = ents[i];
                    n++;
                    break;
                }
    }
    return n;
}

void gt_project_reset(void) { g_nlist = 0, g_list_read = false; } // (the tests: read again)

int gt_selftest(void) {
    int fails = 0;
#define CHECK(c, msg) do { if (!(c)) printf("[RealEngine] FAIL: tags: %s\n", msg), fails++; } while (0)
    CHECK(gt_matches("Damage.Fire", "Damage", false), "a child matches its parent");
    CHECK(!gt_matches("Damage.Fire", "Damage", true), "exact: a child is not its parent");
    CHECK(gt_matches("Damage.Fire", "damage.fire", true), "case-insensitive");
    CHECK(!gt_matches("DamageX", "Damage", false), "a longer name is not a child");
    CHECK(!gt_matches("Damage", "Damage.Fire", false), "a parent doesn't have its child");
    char c[GT_TAG_LEN];
    CHECK(gt_clean(" .State..Stunned. ", c, sizeof c) && !strcmp(c, "State.Stunned"), "the name cleaned");
    CHECK(!gt_clean(" . ", c, sizeof c), "an empty name refused");
    World w;
    if (!ecs_init(&w)) return fails + 1;
    Entity e = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    CHECK(!gt_has(&w, e, "State", false), "no component: no tag");
    CHECK(gt_add(&w, e, "State.Stunned") && gt_add(&w, e, "Team.Red") && gt_add(&w, e, "state.stunned"), "added (once)");
    const CGameplayTags* g = ecs_get(&w, e, COMP_GAMEPLAY_TAGS);
    CHECK(g && g->count == 2, "two tags (the repeated one kept once)");
    CHECK(gt_has(&w, e, "State", false) && gt_has(&w, e, "Team.Red", true) && !gt_has(&w, e, "State", true), "has");
    CHECK(gt_remove(&w, e, "State.Stunned") && !gt_has(&w, e, "State", false) && gt_has(&w, e, "Team", false), "removed");
    CHECK(!gt_remove(&w, e, "State.Stunned"), "removing a missing tag");
    for (int i = 0; i < GT_MAX_TAGS + 2; i++) {
        snprintf(c, sizeof c, "Many.T%d", i);
        bool ok = gt_add(&w, e, c);
        if (i >= GT_MAX_TAGS - 1) CHECK(!ok, "full: refused");
    }
    // the project's list (no project open: kept in memory), the rename on the actors, the search
    gt_project_reset();
    CHECK(gt_project_add("Damage.Fire") && gt_project_add(" damage.ice ") && gt_project_add("State.Stunned") && gt_project_add("Damage.Fire"),
          "project list: added");
    CHECK(gt_project_count() == 3 && !strcmp(gt_project_at(0), "Damage.Fire") && !strcmp(gt_project_at(1), "damage.ice"), "project list: sorted, once");
    Entity a2 = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    Entity a3 = ecs_create(&w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM));
    gt_add(&w, a2, "Damage.Fire"), gt_add(&w, a3, "Damage");
    CHECK(gt_project_rename(&w, "Damage", "Hit") == 4, "renamed: two in the list, two actors");
    CHECK(gt_project_index("Hit.Fire") >= 0 && gt_project_index("hit.ice") >= 0 && gt_project_index("Damage.Fire") < 0, "the list renamed");
    CHECK(gt_has(&w, a2, "Hit.Fire", true) && gt_has(&w, a3, "Hit", true), "the actors renamed");
    Entity found[4];
    CHECK(gt_find_actors(&w, "Hit", false, found, 4) == 2 && gt_find_actors(&w, "Hit", true, found, 4) == 1 && found[0] == a3,
          "found by tag: matching 2, exact 1");
    CHECK(gt_find_actors(&w, "Nope", false, found, 4) == 0, "no actor with a missing tag");
    CHECK(gt_project_remove("Hit") == 2 && gt_project_count() == 1, "removed with what is under it");
    CHECK(gt_project_import_level(&w) >= 2 && gt_project_index("Hit.Fire") >= 0, "the level's tags imported");
    gt_project_reset();
    ecs_shutdown(&w);
#undef CHECK
    printf("[RealEngine] tags: self-test %s\n", fails ? "FAILED" : "ok");
    return fails;
}
