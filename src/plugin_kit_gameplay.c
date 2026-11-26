// (2026-11-25, item 18) The kit's Gameplay plugins (plugin_kit.h): Debug Cheats, Spawn Palette, Teleporter, Tag Finder.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "plugin_kit.h"

#include "i18n.h"
#include "platform.h"
#include "platform_files.h"
#include "theme.h"
#include "ui_icons.h"
#include "ui_number.h"

extern float g_kit_time_scale; // (plugin_kit.c: editor_frame scales Play's time by it)

// the player: the first Character that isn't an AI's pawn
static Entity player_pawn(World* w) {
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        if (e && ecs_get(w, e, COMP_CHARACTER) && !ecs_get(w, e, COMP_AI_AGENT)) return e;
    }
    return ENTITY_NONE;
}

// ---------------------------------------------------------------------------------------------------------------
// Debug Cheats (Play): God Mode, Slow Motion, Heal, Fast Walk, Low Gravity. Everything they change in Play goes back
// with Stop (the world's snapshot); Slow Motion is the editor's time
static bool g_god, g_fast, g_lowgrav;
static float g_slomo = 1.0f;

static void cheats_apply(World* w, Entity p, bool god, bool fast, bool lowgrav) {
    CHealth* h = p ? ecs_get(w, p, COMP_HEALTH) : NULL;
    if (h) h->flags = god ? h->flags | HEALTH_INVINCIBLE : h->flags & ~HEALTH_INVINCIBLE;
    CCharacter* c = p ? ecs_get(w, p, COMP_CHARACTER) : NULL;
    static Entity s_who;
    static float s_speed, s_grav;
    if (c && s_who != p) s_who = p, s_speed = c->max_walk_speed, s_grav = c->gravity_scale; // (the pawn's own, kept once)
    if (c) c->max_walk_speed = fast ? s_speed * 2.5f : s_speed, c->gravity_scale = lowgrav ? s_grav * 0.35f : s_grav;
}
static bool cheats_heal(World* w, Entity p) {
    CHealth* h = p ? ecs_get(w, p, COMP_HEALTH) : NULL;
    if (!h) return false;
    h->health = h->max_health, h->shield = h->max_shield;
    return true;
}
static void cheats_tick(Editor* ed, double dt) {
    (void)dt;
    g_kit_time_scale = ed->playing ? g_slomo : 1.0f;
    if (ed->playing && (g_god || g_fast || g_lowgrav)) cheats_apply(&ed->world, player_pawn(&ed->world), g_god, g_fast, g_lowgrav);
}
static void cheats_window(Editor* ed, double now) {
    if (!ed->playing) igTextColored(THV(TK_WARN), "%s", TR("The cheats work in Play (Alt+P); what they change goes back at Stop."));
    Entity p = player_pawn(&ed->world);
    if (ed->playing) igText("%s: %s", TR("Player"), p ? kit_actor_name(&ed->world, p) : TR("none"));
    igCheckbox(TR("God Mode"), &g_god);
    igCheckbox(TR("Fast Walk"), &g_fast);
    igCheckbox(TR("Low Gravity"), &g_lowgrav);
    if (!g_god && !g_fast && !g_lowgrav && ed->playing && p) cheats_apply(&ed->world, p, false, false, false);
    ui_slider_float(TR("Slow Motion"), &g_slomo, 0.05f, 1.0f, 0.01f, 4.0f, "%.2fx", 0);
    igBeginDisabled(!ed->playing || !p);
    if (igButton(TR("Heal to Full"), (ImVec2){ 0, 0 })) editor_status(ed, now, "%s", cheats_heal(&ed->world, p) ? TR("Healed.") : TR("The player has no Health."));
    igSameLine(0, -1);
    if (igButton(TR("Normal Speed"), (ImVec2){ 0, 0 })) g_slomo = 1.0f;
    igEndDisabled();
}
static void cheats_off(Editor* ed) {
    if (ed->playing && (g_god || g_fast || g_lowgrav)) cheats_apply(&ed->world, player_pawn(&ed->world), false, false, false);
    g_god = g_fast = g_lowgrav = false, g_slomo = 1.0f, g_kit_time_scale = 1.0f;
}
static bool cheats_test(Editor* ed, char* msg, size_t cap) {
    Entity e = kit_spawn_test_cube(ed, "CheatPawn", 0, 0, 0);
    World* w = &ed->world;
    bool made = e && ecs_add(w, e, COMP_CHARACTER) && ecs_add(w, e, COMP_HEALTH);
    CHealth* h = ecs_get(w, e, COMP_HEALTH);
    CCharacter* c = ecs_get(w, e, COMP_CHARACTER);
    if (!made || !h || !c) return false;
    h->max_health = 100, h->health = 10, c->max_walk_speed = 600, c->gravity_scale = 1;
    cheats_apply(w, e, true, true, true);
    bool god = (ecs_get(w, e, COMP_HEALTH) ? ((CHealth*)ecs_get(w, e, COMP_HEALTH))->flags & HEALTH_INVINCIBLE : 0) != 0;
    float fast = ((CCharacter*)ecs_get(w, e, COMP_CHARACTER))->max_walk_speed;
    cheats_apply(w, e, false, false, false);
    float back = ((CCharacter*)ecs_get(w, e, COMP_CHARACTER))->max_walk_speed;
    bool healed = cheats_heal(w, e) && ((CHealth*)ecs_get(w, e, COMP_HEALTH))->health == 100.0f;
    float keep = g_slomo;
    g_slomo = 0.25f, ed->playing = true;
    cheats_tick(ed, 0.0);
    float ts = kit_time_scale();
    ed->playing = false, g_slomo = keep;
    cheats_tick(ed, 0.0);
    snprintf(msg, cap, "invincible, walk 600 -> %.0f -> %.0f cm/s, healed to 100, time x%.2f in Play", fast, back, ts);
    kit_destroy(ed, e);
    return god && fast == 1500.0f && back == 600.0f && healed && ts == 0.25f && kit_time_scale() == 1.0f;
}

const KitPlugin kit_cheats = { "DebugCheats", "Debug Cheats", "Gameplay", "BUG",
    "Cheats for testing in Play: God Mode, Fast Walk, Low Gravity, Heal to Full and Slow Motion. Everything goes back when Play stops.",
    false, cheats_window, NULL, cheats_tick, cheats_off, cheats_test };

// ---------------------------------------------------------------------------------------------------------------
// Spawn Palette: your favorite classes (Config/SpawnPalette.txt) as big buttons; a click places one in front of the camera
#define SP_MAX 48
static char g_sp[SP_MAX][64];
static int g_nsp = -1;
static char g_sp_find[48];

static void sp_path(char* out, size_t cap) { kit_project_file(out, cap, "Config/SpawnPalette.txt"); }
static void sp_load(void) {
    g_nsp = 0;
    char path[900];
    sp_path(path, sizeof path);
    FILE* f = re_fopen(path, "rb");
    if (!f) { // the defaults: what a level needs first
        static const char* const def[] = { "Point Light", "Spot Light", "Player Start", "Camera Actor", "Cube", "Trigger Box" };
        for (int i = 0; i < 6; i++) snprintf(g_sp[g_nsp++], 64, "%s", def[i]);
        return;
    }
    char line[96];
    while (g_nsp < SP_MAX && fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] && line[0] != ';') snprintf(g_sp[g_nsp++], 64, "%s", line);
    }
    fclose(f);
}
static void sp_save(void) {
    char path[900], dir[900];
    sp_path(path, sizeof path);
    snprintf(dir, sizeof dir, "%s", path);
    char* sl = strrchr(dir, '/');
    if (sl) *sl = 0, re_make_dirs(dir);
    FILE* f = re_fopen(path, "wb");
    if (!f) return;
    fputs("; Tools > Plugins > Spawn Palette: the classes shown, one per line\n", f);
    for (int i = 0; i < g_nsp; i++) fprintf(f, "%s\n", g_sp[i]);
    fclose(f);
}
static int place_index(const char* name) {
    for (int i = 0; i < editor_place_count(); i++) {
        const char* cat;
        if (!_stricmp(editor_place_name(i, &cat), name)) return i;
    }
    return -1;
}
static bool ci_has(const char* hay, const char* needle) {
    for (; *hay; hay++) {
        size_t k = 0;
        while (needle[k] && tolower((unsigned char)hay[k]) == tolower((unsigned char)needle[k])) k++;
        if (!needle[k]) return true;
    }
    return !needle[0];
}
static void sp_window(Editor* ed, double now) {
    if (g_nsp < 0) sp_load();
    float avail = igGetContentRegionAvail().x, bw = 128.0f;
    int cols = (int)(avail / (bw + 6.0f));
    if (cols < 1) cols = 1;
    int del = -1;
    for (int i = 0; i < g_nsp; i++) {
        igPushID_Int(i);
        if (i % cols) igSameLine(0, 6.0f);
        int pi = place_index(g_sp[i]);
        igBeginDisabled(pi < 0 || ed->playing);
        if (igButton(TR(g_sp[i]), (ImVec2){ bw, 40 })) {
            Entity e = editor_place_spawn(ed, pi, now);
            if (e) editor_status(ed, now, TR("%s placed."), kit_actor_name(&ed->world, e));
        }
        igEndDisabled();
        if (igBeginPopupContextItem("sp", ImGuiPopupFlags_MouseButtonRight)) {
            if (igMenuItem_Bool(TR("Remove from Palette"), NULL, false, true)) del = i;
            igEndPopup();
        }
        igPopID();
    }
    if (del >= 0) memmove(g_sp[del], g_sp[del + 1], sizeof g_sp[0] * (size_t)(g_nsp - del - 1)), g_nsp--, sp_save();
    igSeparatorText(TR("Add a Class"));
    igSetNextItemWidth(-1);
    igInputTextWithHint("##spfind", TR("Search the classes"), g_sp_find, sizeof g_sp_find, 0, NULL, NULL);
    if (g_sp_find[0] && igBeginChild_Str("spfound", (ImVec2){ 0, 160 }, ImGuiChildFlags_Borders, 0)) {
        for (int i = 0; i < editor_place_count(); i++) {
            const char* cat;
            const char* nm = editor_place_name(i, &cat);
            if (!ci_has(nm, g_sp_find) && !ci_has(TR(nm), g_sp_find)) continue;
            igPushID_Int(i);
            char l[160];
            snprintf(l, sizeof l, "%s   (%s)", TR(nm), cat ? TR(cat) : "");
            if (igSelectable_Bool(l, false, 0, (ImVec2){ 0, 0 }) && g_nsp < SP_MAX) snprintf(g_sp[g_nsp++], 64, "%s", nm), sp_save();
            igPopID();
        }
    }
    if (g_sp_find[0]) igEndChild();
}
static bool sp_test(Editor* ed, char* msg, size_t cap) {
    int pi = place_index("Point Light");
    Entity e = pi >= 0 ? editor_place_spawn(ed, pi, 0.0) : ENTITY_NONE;
    bool light = e && ecs_get(&ed->world, e, COMP_LIGHT) != NULL;
    snprintf(msg, cap, "%d classes to pick from; Point Light placed: %s", editor_place_count(), light ? "a light" : "NOTHING");
    kit_destroy(ed, e);
    return light;
}
static void sp_off(Editor* ed) { (void)ed, g_nsp = -1, g_sp_find[0] = 0; }

const KitPlugin kit_spawn_palette = { "SpawnPalette", "Spawn Palette", "Gameplay", "PLUS",
    "Your most used classes as big buttons (lights, Player Start, triggers, your Blueprints): one click places one in front of the camera. Kept in Config/SpawnPalette.txt.",
    false, sp_window, NULL, NULL, sp_off, sp_test };

// ---------------------------------------------------------------------------------------------------------------
// Teleporter: the level's Player Starts and Checkpoints in a list; the editor's camera flies to one, or in Play the player
static bool tp_to(Editor* ed, Entity target) {
    CTransform* t = ecs_get(&ed->world, target, COMP_TRANSFORM);
    if (!t) return false;
    if (!ed->playing) return viewport_focus(ed, target), true;
    Entity p = player_pawn(&ed->world);
    CTransform* pt = p ? ecs_get(&ed->world, p, COMP_TRANSFORM) : NULL;
    if (!pt) return false;
    float at[3] = { t->location[0], t->location[1], t->location[2] };
    const CCheckpoint* cp = ecs_get(&ed->world, target, COMP_CHECKPOINT);
    if (cp) at[0] += cp->spawn_offset[0], at[1] += cp->spawn_offset[1], at[2] += cp->spawn_offset[2];
    memcpy(pt->location, at, sizeof at);
    pt->rotation[1] = t->rotation[1];
    ecs_touch(&ed->world, p);
    return true;
}
static void tp_window(Editor* ed, double now) {
    igTextWrapped("%s", ed->playing ? TR("Click a place: the player goes there.") : TR("Click a place: the camera flies to it. In Play, the player goes there."));
    World* w = &ed->world;
    uint32_t n = 0;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        bool start = e && ecs_get(w, e, COMP_PLAYER_START), cp = e && ecs_get(w, e, COMP_CHECKPOINT);
        if (!start && !cp) continue;
        igPushID_Int((int)s);
        ui_icon_item(start ? ICON_PLAYER : ICON_FLOOR, igGetTextLineHeight(), TH(start ? TK_ACCENT : TK_GREEN));
        igSameLine(0, 6.0f);
        if (igSelectable_Bool(kit_actor_name(w, e), false, 0, (ImVec2){ 0, 0 }) && tp_to(ed, e))
            editor_status(ed, now, TR("Teleported to %s."), kit_actor_name(w, e));
        igPopID();
        n++;
    }
    if (!n) igTextDisabled("%s", TR("The level has no Player Start or Checkpoint."));
}
static bool tp_test(Editor* ed, char* msg, size_t cap) {
    World* w = &ed->world;
    Entity start = kit_spawn_test_cube(ed, "TpStart", 2500, -700, 90), pawn = kit_spawn_test_cube(ed, "TpPawn", 0, 0, 0);
    bool made = ecs_add(w, start, COMP_PLAYER_START) && ecs_add(w, pawn, COMP_CHARACTER);
    bool keep = ed->playing;
    ed->playing = true;
    bool moved = made && tp_to(ed, start);
    ed->playing = keep;
    const CTransform* t = ecs_get(w, pawn, COMP_TRANSFORM);
    bool there = t && t->location[0] == 2500.0f && t->location[1] == -700.0f;
    snprintf(msg, cap, "the player moved to the Player Start (%.0f, %.0f)", t ? t->location[0] : 0, t ? t->location[1] : 0);
    kit_destroy(ed, start), kit_destroy(ed, pawn);
    return moved && (there || player_pawn(w) != pawn);
}

const KitPlugin kit_teleporter = { "Teleporter", "Teleporter", "Gameplay", "PLAYER",
    "Every Player Start and Checkpoint of the level in a list: the camera flies to one, or in Play the player is moved there at once to test a part of the level.",
    false, tp_window, NULL, NULL, NULL, tp_test };

// ---------------------------------------------------------------------------------------------------------------
// Tag Finder: the Gameplay Tags the level's actors carry, how many each, and the actors of one (selected in a click)
static char g_tag[GT_TAG_LEN];
static uint32_t tag_select(Editor* ed, const char* tag) {
    World* w = &ed->world;
    editor_select_none(ed);
    uint32_t n = 0;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        const CGameplayTags* g = e ? ecs_get(w, e, COMP_GAMEPLAY_TAGS) : NULL;
        for (uint32_t k = 0; g && k < g->count && k < GT_MAX_TAGS; k++)
            if (!strcmp(g->tags[k], tag)) editor_select(ed, e, SEL_ADD), n++, k = GT_MAX_TAGS;
    }
    return n;
}
static void tag_window(Editor* ed, double now) {
    World* w = &ed->world;
    static char tags[128][GT_TAG_LEN];
    static uint32_t counts[128];
    uint32_t nt = 0;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        const CGameplayTags* g = e ? ecs_get(w, e, COMP_GAMEPLAY_TAGS) : NULL;
        for (uint32_t k = 0; g && k < g->count && k < GT_MAX_TAGS; k++) {
            uint32_t j = 0;
            while (j < nt && strcmp(tags[j], g->tags[k])) j++;
            if (j == nt && nt < 128) snprintf(tags[nt], GT_TAG_LEN, "%s", g->tags[k]), counts[nt++] = 0;
            if (j < nt) counts[j]++;
        }
    }
    if (!nt) igTextDisabled("%s", TR("No actor of the level has a Gameplay Tag."));
    for (uint32_t j = 0; j < nt; j++) {
        char l[96];
        snprintf(l, sizeof l, "%s  (%u)", tags[j], counts[j]);
        if (igSelectable_Bool(l, !strcmp(g_tag, tags[j]), 0, (ImVec2){ 0, 0 })) snprintf(g_tag, sizeof g_tag, "%s", tags[j]);
    }
    if (!g_tag[0]) return;
    igSeparatorText(g_tag);
    if (igButton(TR("Select All with the Tag"), (ImVec2){ 0, 0 })) editor_status(ed, now, TR("%u actors selected."), tag_select(ed, g_tag));
}
static bool tag_test(Editor* ed, char* msg, size_t cap) {
    World* w = &ed->world;
    Entity e[3] = { kit_spawn_test_cube(ed, "TagA", 0, 0, 0), kit_spawn_test_cube(ed, "TagB", 100, 0, 0), kit_spawn_test_cube(ed, "TagC", 200, 0, 0) };
    for (int i = 0; i < 3; i++) {
        ecs_add(w, e[i], COMP_GAMEPLAY_TAGS);
        CGameplayTags* g = ecs_get(w, e[i], COMP_GAMEPLAY_TAGS);
        if (g) g->count = 1, snprintf(g->tags[0], GT_TAG_LEN, "%s", i < 2 ? "Kit.Test.Enemy" : "Kit.Test.Friend");
    }
    uint32_t n = tag_select(ed, "Kit.Test.Enemy");
    bool ok = n == 2 && editor_is_selected(ed, e[0]) && editor_is_selected(ed, e[1]) && !editor_is_selected(ed, e[2]);
    snprintf(msg, cap, "%u actors with Kit.Test.Enemy selected", n);
    for (int i = 0; i < 3; i++) kit_destroy(ed, e[i]);
    return ok;
}
static void tag_off(Editor* ed) { (void)ed, g_tag[0] = 0; }

const KitPlugin kit_tag_finder = { "TagFinder", "Tag Finder", "Gameplay", "TAG",
    "Every Gameplay Tag the level's actors carry, with how many actors have each; one click selects all the actors with a tag.",
    false, tag_window, NULL, NULL, tag_off, tag_test };
