// (2026-11-25, item 18) The kit's Rendering plugins (plugin_kit.h): Screenshot, Lighting Audit, Scene Stats, Light Radius.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "plugin_kit.h"

#include "debug_draw.h"
#include "i18n.h"
#include "platform.h"
#include "platform_files.h"
#include "theme.h"
#include "ui_number.h"
#include "vkctx.h"

// ---------------------------------------------------------------------------------------------------------------
// Screenshot (F9): the viewport alone (the icons and grid hidden for that frame) or the whole window, as a PNG in
// Saved/Screenshots with the date in its name
static int g_shot_state; // 0 idle, 1 hiding the icons, 2 waiting for the grab
static bool g_shot_window, g_shot_hide = true, g_shot_keep_game;
static char g_shot_last[900];

static void shot_name(char* out, size_t cap) {
    char dir[900];
    kit_project_file(dir, sizeof dir, "Saved/Screenshots");
    re_make_dirs(dir);
    time_t t = time(NULL);
    struct tm tmv;
    localtime_s(&tmv, &t);
    for (int i = 0; i < 100; i++) {
        snprintf(out, cap, "%s/Shot_%04d-%02d-%02d_%02d-%02d-%02d%s%.0d.png", dir, tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min,
                 tmv.tm_sec, i ? "_" : "", i);
        if (!re_file_stat(out, NULL, NULL, NULL)) return;
    }
}
static bool shot_begin(Editor* ed) {
    if (g_shot_state || !ed->vk || ed->vk->grab_req || ed->vk->grab_ready) return false;
    shot_name(g_shot_last, sizeof g_shot_last);
    if (g_shot_window) return vkctx_request_shot(ed->vk, g_shot_last), true;
    g_shot_keep_game = ed->game_view;
    if (g_shot_hide) ed->game_view = true;
    g_shot_state = 1;
    return true;
}
static void shot_tick(Editor* ed, double dt) {
    (void)dt;
    if (!igGetIO_Nil()->WantTextInput && igIsKeyPressed_Bool(ImGuiKey_F9, false) && shot_begin(ed))
        editor_status(ed, re_time_seconds(), TR("Screenshot: %s"), g_shot_last);
    if (g_shot_state == 1) { // (a frame drawn without the icons first)
        const ImGuiViewport* vp = igGetMainViewport();
        float r[4] = { ed->vp_rect[0] - vp->Pos.x, ed->vp_rect[1] - vp->Pos.y, ed->vp_rect[2], ed->vp_rect[3] };
        vkctx_request_grab(ed->vk, r);
        g_shot_state = 2;
    } else if (g_shot_state == 2 && ed->vk->grab_ready) {
        bool ok = vkctx_png_write_rgb(g_shot_last, ed->vk->grab_px, ed->vk->grab_w, ed->vk->grab_h);
        ed->vk->grab_ready = false;
        ed->game_view = g_shot_keep_game;
        g_shot_state = 0;
        editor_status(ed, re_time_seconds(), ok ? TR("Screenshot saved: %s") : TR("The screenshot couldn't be written: %s"), g_shot_last);
    }
}
static void shot_window(Editor* ed, double now) {
    igTextWrapped("%s", TR("F9 (or the button) saves a PNG of the viewport, or of the whole window, in Saved/Screenshots."));
    igCheckbox(TR("The Whole Window"), &g_shot_window);
    igBeginDisabled(g_shot_window);
    igCheckbox(TR("Hide Icons and Grid"), &g_shot_hide);
    igEndDisabled();
    if (igButton(TR("Take Screenshot"), (ImVec2){ -1, 0 }) && shot_begin(ed)) editor_status(ed, now, TR("Screenshot: %s"), g_shot_last);
    if (g_shot_last[0]) {
        igTextDisabled("%s", g_shot_last);
        if (igButton(TR("Show in Explorer"), (ImVec2){ 0, 0 })) re_show_in_explorer(g_shot_last);
    }
}
static void shot_off(Editor* ed) {
    if (g_shot_state) ed->game_view = g_shot_keep_game, g_shot_state = 0;
}
static bool shot_test(Editor* ed, char* msg, size_t cap) {
    (void)ed;
    char path[900];
    shot_name(path, sizeof path);
    uint8_t px[4 * 4 * 3];
    for (int i = 0; i < 48; i++) px[i] = (uint8_t)(i * 5);
    bool ok = vkctx_png_write_rgb(path, px, 4, 4);
    uint64_t size = 0;
    ok = ok && re_file_stat(path, &size, NULL, NULL) && size > 40;
    snprintf(msg, cap, "a PNG named by the date written in Saved/Screenshots (%llu bytes)", (unsigned long long)size);
    remove(path);
    return ok;
}

const KitPlugin kit_screenshot = { "Screenshot", "Screenshot", "Rendering", "CAMERA",
    "F9 saves a PNG of the viewport (the icons and grid hidden for that frame) or of the whole window in Saved/Screenshots, named by the date.",
    false, shot_window, NULL, shot_tick, shot_off, shot_test };

// ---------------------------------------------------------------------------------------------------------------
// Lighting Audit: the level's lights in a table, and what costs or looks wrong (many shadowed lights overlapping, two
// suns, no sky light, a light at 0, a huge radius)
typedef struct LightIssue { uint32_t lights, shadowed, suns, skies, dark, huge, crowded; } LightIssue;
static LightIssue light_audit(World* w) {
    LightIssue r = { 0 };
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        const CLight* l = e ? ecs_get(w, e, COMP_LIGHT) : NULL;
        if (!l) continue;
        r.lights++;
        r.shadowed += l->cast_shadows != 0;
        r.suns += l->type == LIGHT_DIRECTIONAL;
        r.skies += l->type == LIGHT_SKY;
        r.dark += l->intensity <= 0.0f;
        bool local = l->type == LIGHT_POINT || l->type == LIGHT_SPOT || l->type == LIGHT_RECT;
        r.huge += local && l->attenuation_radius > 10000.0f;
        if (!local || !l->cast_shadows) continue;
        const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        uint32_t near = 0; // (shadowed local lights whose spheres overlap this one's)
        for (uint32_t s2 = 1; t && s2 < w->slot_count; s2++) {
            Entity o = ecs_entity_at(w, s2);
            const CLight* l2 = o && o != e ? ecs_get(w, o, COMP_LIGHT) : NULL;
            const CTransform* t2 = l2 ? ecs_get(w, o, COMP_TRANSFORM) : NULL;
            if (!t2 || !l2->cast_shadows || l2->type == LIGHT_DIRECTIONAL || l2->type == LIGHT_SKY) continue;
            float dx = t->location[0] - t2->location[0], dy = t->location[1] - t2->location[1], dz = t->location[2] - t2->location[2];
            float rr = l->attenuation_radius + l2->attenuation_radius;
            near += dx * dx + dy * dy + dz * dz < rr * rr;
        }
        r.crowded += near >= 4;
    }
    return r;
}
static void light_audit_window(Editor* ed, double now) {
    (void)now;
    World* w = &ed->world;
    LightIssue r = light_audit(w);
    igText(TR("%u lights, %u cast shadows"), r.lights, r.shadowed);
    int issues = 0;
    if (r.suns > 1) igTextColored(THV(TK_WARN), TR("%u Directional Lights: one sun is usual (each one costs its shadows)."), r.suns), issues++;
    if (!r.skies && r.lights) igTextColored(THV(TK_INFO), "%s", TR("No Sky Light: the shadows get no light from the sky.")), issues++;
    if (r.dark) igTextColored(THV(TK_WARN), TR("%u lights at intensity 0 (they still cost)."), r.dark), issues++;
    if (r.huge) igTextColored(THV(TK_WARN), TR("%u local lights reach farther than 100 m."), r.huge), issues++;
    if (r.crowded) igTextColored(THV(TK_WARN), TR("%u shadowed lights overlap 4 others or more: turn off some shadows."), r.crowded), issues++;
    if (!issues) igTextColored(THV(TK_OK), "%s", TR("Nothing to fix."));
    static const char* const types[LIGHT_TYPE_COUNT] = { "Directional", "Point", "Spot", "Rect", "Sky" };
    if (igBeginTable("lights", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY, (ImVec2){ 0, 0 }, 0)) {
        igTableSetupScrollFreeze(0, 1);
        igTableSetupColumn(TR("Light"), 0, 0, 0);
        igTableSetupColumn(TR("Type"), 0, 0, 0);
        igTableSetupColumn(TR("Intensity"), 0, 0, 0);
        igTableSetupColumn(TR("Radius"), 0, 0, 0);
        igTableSetupColumn(TR("Shadows"), 0, 0, 0);
        igTableHeadersRow();
        for (uint32_t s = 1; s < w->slot_count; s++) {
            Entity e = ecs_entity_at(w, s);
            const CLight* l = e ? ecs_get(w, e, COMP_LIGHT) : NULL;
            if (!l) continue;
            igTableNextRow(0, 0);
            igTableNextColumn();
            igPushID_Int((int)s);
            if (igSelectable_Bool(kit_actor_name(w, e), editor_is_selected(ed, e), ImGuiSelectableFlags_SpanAllColumns, (ImVec2){ 0, 0 }))
                editor_select(ed, e, SEL_REPLACE), viewport_focus(ed, e);
            igPopID();
            igTableNextColumn(), igTextUnformatted(TR(types[l->type < LIGHT_TYPE_COUNT ? l->type : 1]), NULL);
            igTableNextColumn(), igText("%.1f", l->intensity);
            igTableNextColumn();
            l->type == LIGHT_DIRECTIONAL || l->type == LIGHT_SKY ? igTextDisabled("-") : igText("%.0f cm", l->attenuation_radius);
            igTableNextColumn(), igTextUnformatted(l->cast_shadows ? TR("yes") : "-", NULL);
        }
        igEndTable();
    }
}
static Entity place_named(Editor* ed, const char* name) {
    for (int i = 0; i < editor_place_count(); i++) {
        const char* cat;
        if (!_stricmp(editor_place_name(i, &cat), name)) return editor_place_spawn(ed, i, 0.0);
    }
    return ENTITY_NONE;
}
static bool light_audit_test(Editor* ed, char* msg, size_t cap) {
    LightIssue before = light_audit(&ed->world);
    Entity a = place_named(ed, "Directional Light"), b = place_named(ed, "Directional Light"), c = place_named(ed, "Point Light");
    CLight* lc = c ? ecs_get(&ed->world, c, COMP_LIGHT) : NULL;
    if (lc) lc->intensity = 0.0f;
    LightIssue r = light_audit(&ed->world);
    snprintf(msg, cap, "%u lights: %u suns, %u at intensity 0 flagged", r.lights, r.suns, r.dark);
    kit_destroy(ed, a), kit_destroy(ed, b), kit_destroy(ed, c);
    return a && b && c && r.suns == before.suns + 2 && r.dark == before.dark + 1 && r.lights == before.lights + 3;
}

const KitPlugin kit_light_audit = { "LightingAudit", "Lighting Audit", "Rendering", "BULB",
    "The level's lights in a table (type, intensity, radius, shadows) and what costs or looks wrong: two suns, no Sky Light, lights at 0, huge radii, too many shadowed lights overlapping.",
    false, light_audit_window, NULL, NULL, NULL, light_audit_test };

// ---------------------------------------------------------------------------------------------------------------
// Scene Stats: a panel over the viewport with the frame's times and what the level holds
typedef struct SceneCount { uint32_t actors, meshes, skeletal, lights, shadowed, particles, audio, ai, hidden; } SceneCount;
static bool g_ss_counts = true, g_ss_times = true;
static SceneCount scene_count(World* w) {
    SceneCount c = { 0 };
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        if (!e) continue;
        uint64_t m = ecs_mask(w, e);
        c.actors++;
        c.meshes += (m & COMP_BIT(COMP_STATIC_MESH)) != 0;
        c.skeletal += (m & COMP_BIT(COMP_SKELETAL_MESH)) != 0;
        c.particles += (m & COMP_BIT(COMP_PARTICLE_SYSTEM)) != 0;
        c.audio += (m & COMP_BIT(COMP_AUDIO)) != 0;
        c.ai += (m & COMP_BIT(COMP_AI_AGENT)) != 0;
        const CLight* l = (m & COMP_BIT(COMP_LIGHT)) ? ecs_get(w, e, COMP_LIGHT) : NULL;
        if (l) c.lights++, c.shadowed += l->cast_shadows != 0;
        const CTransform* t = ecs_get(w, e, COMP_TRANSFORM);
        c.hidden += t && (t->flags & TRANSFORM_HIDDEN);
    }
    return c;
}
static void stats_overlay(Editor* ed, struct ImDrawList* dl) {
    static SceneCount c;
    static double at;
    if (re_time_seconds() - at > 0.25) c = scene_count(&ed->world), at = re_time_seconds(); // (four times a second: a walk of the slots)
    char t[512];
    size_t o = 0;
    if (g_ss_times)
        o += (size_t)snprintf(t + o, sizeof t - o, "%.0f fps  %.2f ms\nCPU %.2f ms  GPU %.2f ms\n", ed->stats.fps, ed->stats.frame_ms, ed->stats.cpu_ms, ed->stats.gpu_ms);
    if (g_ss_counts)
        o += (size_t)snprintf(t + o, sizeof t - o, "%s %u  %s %u  %s %u\n%s %u (%u %s)\n%s %u  %s %u  IA %u", TR("Actors"), c.actors, TR("Meshes"), c.meshes,
                              TR("Skeletal"), c.skeletal, TR("Lights"), c.lights, c.shadowed, TR("shadowed"), TR("Particles"), c.particles, TR("Sounds"), c.audio, c.ai);
    if (o && t[o - 1] == '\n') t[o - 1] = 0;
    if (o) kit_hud(dl, ed, TK_AMBER, TR("Scene Stats"), t);
}
static void stats_window(Editor* ed, double now) {
    (void)ed, (void)now;
    igTextWrapped("%s", TR("A panel at the viewport's top right with the frame's times and what the level holds (actors, meshes, lights, shadows, particles, sounds, AI)."));
    igCheckbox(TR("Frame Times"), &g_ss_times);
    igCheckbox(TR("Scene Counts"), &g_ss_counts);
}
static bool stats_test(Editor* ed, char* msg, size_t cap) {
    SceneCount a = scene_count(&ed->world);
    Entity l = place_named(ed, "Point Light"), m = kit_spawn_test_cube(ed, "StatsCube", 0, 0, 0);
    SceneCount b = scene_count(&ed->world);
    snprintf(msg, cap, "%u actors, +1 light, +1 mesh counted", b.actors);
    kit_destroy(ed, l), kit_destroy(ed, m);
    return b.actors == a.actors + 2 && b.lights == a.lights + 1 && b.meshes == a.meshes + 1;
}

const KitPlugin kit_scene_stats = { "SceneStats", "Scene Stats", "Rendering", "STATS",
    "A panel over the viewport with the frame's fps, CPU and GPU time and what the level holds: actors, meshes, Skeletal Meshes, lights and shadowed lights, particles, sounds, AI.",
    false, stats_window, stats_overlay, NULL, NULL, stats_test };

// ---------------------------------------------------------------------------------------------------------------
// Light Radius: the local lights' reach drawn in the viewport (a sphere for points, the cone for spots) in their color
static bool g_lr_all;
static uint32_t light_radius_draw(Editor* ed) {
    World* w = &ed->world;
    uint32_t n = 0;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        const CLight* l = e ? ecs_get(w, e, COMP_LIGHT) : NULL;
        const CTransform* t = l ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (!t || (l->type != LIGHT_POINT && l->type != LIGHT_SPOT && l->type != LIGHT_RECT) || (!g_lr_all && !editor_is_selected(ed, e))) continue;
        uint32_t col = dd_rgba(l->color[0], l->color[1], l->color[2], 0.7f);
        if (l->type == LIGHT_SPOT) { // the cone: its axis and the rim at the radius
            float ax[3][3];
            rotator_axes(t->rotation, ax);
            float r = l->attenuation_radius, a = l->outer_cone * 3.14159265f / 180.0f;
            float c[3] = { t->location[0] + ax[0][0] * r * cosf(a), t->location[1] + ax[0][1] * r * cosf(a), t->location[2] + ax[0][2] * r * cosf(a) };
            float rim = r * sinf(a), prev[3] = { 0 };
            for (int i = 0; i <= 24; i++) {
                float u = (float)i / 24.0f * 6.2831853f, p[3];
                for (int k = 0; k < 3; k++) p[k] = c[k] + (ax[1][k] * cosf(u) + ax[2][k] * sinf(u)) * rim;
                if (i) dd_line(prev, p, col, 1.5f, 0.0f);
                if (!(i % 6)) dd_line(t->location, p, col, 1.0f, 0.0f);
                memcpy(prev, p, sizeof prev);
            }
        } else dd_sphere(t->location, l->attenuation_radius, col, 1.5f, 0.0f);
        n++;
    }
    return n;
}
static void lr_tick(Editor* ed, double dt) {
    (void)dt;
    if (!ed->game_view) light_radius_draw(ed);
}
static void lr_window(Editor* ed, double now) {
    (void)ed, (void)now;
    igTextWrapped("%s", TR("Draws how far the local lights reach: a sphere for Point and Rect Lights, the cone for Spot Lights, in each light's color."));
    igCheckbox(TR("Every Light (not only the selected)"), &g_lr_all);
}
static bool lr_test(Editor* ed, char* msg, size_t cap) {
    Entity a = place_named(ed, "Point Light"), b = place_named(ed, "Spot Light");
    editor_select(ed, a, SEL_REPLACE);
    bool keep = g_lr_all;
    g_lr_all = false;
    uint32_t sel = light_radius_draw(ed);
    g_lr_all = true;
    uint32_t all = light_radius_draw(ed);
    g_lr_all = keep;
    snprintf(msg, cap, "%u drawn for the selection, %u for every light", sel, all);
    kit_destroy(ed, a), kit_destroy(ed, b);
    return a && b && sel == 1 && all >= 2;
}

const KitPlugin kit_light_radius = { "LightRadius", "Light Radius", "Rendering", "SPOT",
    "Shows in the viewport how far the local lights reach: a sphere for Point and Rect Lights and the cone for Spot Lights, in each light's color (the selected, or all).",
    false, lr_window, NULL, lr_tick, NULL, lr_test };
