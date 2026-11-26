// (2026-11-25, item 18) The editor's built-in plugins (plugin_kit.h): the registry, Tools > Plugins, their windows and
// overlays each frame, their memory and the self-tests. The plugins themselves: plugin_kit_*.c, one file per category.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "plugin_kit.h"

#include "blueprint.h"
#include "content.h"
#include "i18n.h"
#include "landscape.h"
#include "render.h"
#include "platform_files.h"
#include "platform.h"
#include "plugins.h"
#include "project.h"
#include "scene_query.h"
#include "theme.h"
#include "ui_icons.h"

// the plugins, by category (each defined in its plugin_kit_<category>.c)
extern const KitPlugin kit_batch_rename, kit_color_palette, kit_material_swap, kit_mesh_audit;
extern const KitPlugin kit_scatter, kit_snap_ground, kit_align, kit_array, kit_measure;
extern const KitPlugin kit_anim_rate, kit_anim_audit, kit_motion_trails;
extern const KitPlugin kit_cheats, kit_spawn_palette, kit_teleporter, kit_tag_finder;
extern const KitPlugin kit_cam_bookmarks, kit_actor_notes, kit_selection_sets, kit_command_palette, kit_asset_usage, kit_unused_assets;
extern const KitPlugin kit_screenshot, kit_light_audit, kit_scene_stats, kit_light_radius;
extern const KitPlugin kit_sound_audit, kit_sound_map;
extern const KitPlugin kit_ai_overview, kit_nav_tester;
extern const KitPlugin kit_safe_frames;
extern const KitPlugin kit_net_stats;

static const KitPlugin* const g_kit[] = {
    &kit_batch_rename, &kit_color_palette, &kit_material_swap, &kit_mesh_audit,
    &kit_scatter, &kit_snap_ground, &kit_align, &kit_array, &kit_measure,
    &kit_anim_rate, &kit_anim_audit, &kit_motion_trails,
    &kit_cheats, &kit_spawn_palette, &kit_teleporter, &kit_tag_finder,
    &kit_cam_bookmarks, &kit_actor_notes, &kit_selection_sets, &kit_command_palette, &kit_asset_usage, &kit_unused_assets,
    &kit_screenshot, &kit_light_audit, &kit_scene_stats, &kit_light_radius,
    &kit_sound_audit, &kit_sound_map,
    &kit_ai_overview, &kit_nav_tester,
    &kit_safe_frames,
    &kit_net_stats,
};
#define KIT_N ((uint32_t)(sizeof g_kit / sizeof g_kit[0]))

static int g_idx[KIT_N];   // its PluginInfo
static bool g_was[KIT_N];  // on last frame
static bool g_open[KIT_N]; // its window
static bool g_inited;
static size_t g_live;
static float g_hud_y;
float g_kit_time_scale = 1.0f; // (plugin_kit_gameplay.c's Slow Motion)

void* kit_alloc(size_t n) {
    void* p = n ? calloc(1, n) : NULL; // (an editor tool's, not a frame's)
    if (p) g_live += n;
    return p;
}
void* kit_realloc(void* p, size_t old_n, size_t n) {
    void* q = realloc(p, n);
    if (!q) return NULL;
    if (n > old_n) memset((char*)q + old_n, 0, n - old_n);
    g_live = g_live - (p ? old_n : 0) + n;
    return q;
}
void kit_free(void* p, size_t n) {
    if (!p) return;
    free(p);
    g_live -= n;
}
size_t kit_live_bytes(void) { return g_live; }
void kit_account(long long bytes) { g_live = (size_t)((long long)g_live + bytes); }

void kit_init(void) {
    if (g_inited) return;
    g_inited = true;
    for (uint32_t i = 0; i < KIT_N; i++) {
        const KitPlugin* k = g_kit[i];
        g_idx[i] = plugins_add_builtin(k->name, k->friendly, k->category, k->icon, "Rivora Engine", k->description, k->on_default);
    }
}

uint32_t kit_count(void) { return KIT_N; }
const KitPlugin* kit_at(uint32_t i) { return i < KIT_N ? g_kit[i] : NULL; }
bool kit_on(uint32_t i) { return i < KIT_N && plugins_builtin_on(g_idx[i]); }
void kit_open(uint32_t i, bool open) {
    if (i < KIT_N) g_open[i] = open && kit_on(i);
}
bool kit_window_open(const char* name) {
    for (uint32_t i = 0; i < KIT_N; i++)
        if (!strcmp(g_kit[i]->name, name)) return g_open[i] && g_was[i];
    return false;
}
float kit_time_scale(void) { return g_kit_time_scale > 0.0f ? g_kit_time_scale : 1.0f; }

uint32_t kit_menu_count(void) {
    uint32_t n = 0;
    for (uint32_t i = 0; i < KIT_N; i++) n += kit_on(i);
    return n;
}

static UiIcon icon_named(const char* name) {
    for (int k = 0; k < ICON_COUNT; k++)
        if (!_stricmp(ui_icon_name((UiIcon)k), name)) return (UiIcon)k;
    return ICON_PLUGIN;
}

void kit_menu(Editor* ed) {
    (void)ed;
    static const char* const order[] = { "Artist", "Level Design", "Animation", "Gameplay", "Productivity", "Rendering", "Audio", "AI", "UI", "Online" };
    static const UiIcon icons[] = { ICON_BRUSH, ICON_LANDSCAPE, ICON_ANIMATION, ICON_GAMEPAD, ICON_CHECK, ICON_VM_LIT, ICON_SOUND, ICON_BRAIN, ICON_WIDGET, ICON_NETWORK };
    for (int c = 0; c < (int)(sizeof order / sizeof order[0]); c++) {
        uint32_t n = 0;
        for (uint32_t i = 0; i < KIT_N; i++) n += kit_on(i) && !strcmp(g_kit[i]->category, order[c]);
        if (!n || !ui_begin_menu(icons[c], TR(order[c]), true)) continue;
        for (uint32_t i = 0; i < KIT_N; i++)
            if (kit_on(i) && !strcmp(g_kit[i]->category, order[c]) && ui_menu_item(icon_named(g_kit[i]->icon), TR(g_kit[i]->friendly), NULL, g_open[i], true))
                g_open[i] = !g_open[i];
        igEndMenu();
    }
    if (kit_menu_count()) igSeparator();
}

// on -> off: its off() (memory, effects); its window closed
static void kit_sync(Editor* ed) {
    for (uint32_t i = 0; i < KIT_N; i++) {
        bool on = kit_on(i);
        if (g_was[i] && !on) {
            if (g_kit[i]->off) g_kit[i]->off(ed);
            g_open[i] = false;
        }
        g_was[i] = on;
    }
}

void kit_frame(Editor* ed, double dt, double now) {
    kit_init();
    kit_sync(ed);
    bool popup = igIsPopupOpen_Str("", ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    ImDrawList* dl = igGetForegroundDrawList_ViewportPtr(igGetMainViewport());
    const float* r = ed->vp_rect;
    g_hud_y = r[1] + 44.0f;
    for (uint32_t i = 0; i < KIT_N; i++) {
        if (!g_was[i]) continue;
        const KitPlugin* k = g_kit[i];
        if (k->tick) k->tick(ed, dt);
        if (k->overlay && r[2] > 8.0f && r[3] > 8.0f && !popup && !ed->fullscreen) {
            ImDrawList_PushClipRect(dl, (ImVec2){ r[0], r[1] }, (ImVec2){ r[0] + r[2], r[1] + r[3] }, true);
            k->overlay(ed, dl);
            ImDrawList_PopClipRect(dl);
        }
        if (!g_open[i] || !k->window) continue;
        igSetNextWindowSize((ImVec2){ 440, 460 }, ImGuiCond_FirstUseEver);
        char title[160];
        snprintf(title, sizeof title, "%s###kit_%s", TR(k->friendly), k->name);
        if (igBegin(title, &g_open[i], 0)) k->window(ed, now);
        igEnd();
    }
}

// --- helpers ---
void kit_hud(ImDrawList* dl, Editor* ed, int accent, const char* title, const char* text) {
    const float* r = ed->vp_rect;
    float pad = 8.0f, w = 0.0f;
    ImVec2 ts = igCalcTextSize(title, NULL, false, -1), bs = igCalcTextSize(text, NULL, false, -1);
    w = (ts.x > bs.x ? ts.x : bs.x) + pad * 2.0f;
    float h = ts.y + bs.y + pad * 2.0f + 4.0f;
    float x1 = r[0] + r[2] - 12.0f, x0 = x1 - w, y0 = g_hud_y;
    ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x1, y0 + h }, THA(TK_BG_DEEP, 200), 6.0f, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2){ x0, y0 }, (ImVec2){ x0 + 3.0f, y0 + h }, TH(accent), 6.0f, ImDrawFlags_RoundCornersLeft);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ x0 + pad, y0 + pad }, TH(accent), title, NULL);
    ImDrawList_AddText_Vec2(dl, (ImVec2){ x0 + pad, y0 + pad + ts.y + 4.0f }, TH(TK_TEXT), text, NULL);
    g_hud_y += h + 6.0f;
}

bool kit_label(ImDrawList* dl, Editor* ed, const float p[3], uint32_t col, const char* text) {
    float s[2];
    if (!kit_world_to_screen(ed, p, s)) return false;
    ImDrawList_AddCircleFilled(dl, (ImVec2){ s[0], s[1] }, 3.5f, col, 10);
    if (text && text[0]) {
        ImVec2 ts = igCalcTextSize(text, NULL, false, -1);
        ImDrawList_AddRectFilled(dl, (ImVec2){ s[0] + 6.0f, s[1] - ts.y * 0.5f - 2.0f }, (ImVec2){ s[0] + 12.0f + ts.x, s[1] + ts.y * 0.5f + 2.0f },
                                 THA(TK_BG_DEEP, 180), 3.0f, 0);
        ImDrawList_AddText_Vec2(dl, (ImVec2){ s[0] + 9.0f, s[1] - ts.y * 0.5f }, col, text, NULL);
    }
    return true;
}
const char* kit_actor_name(World* w, Entity e) {
    const CName* n = ecs_get(w, e, COMP_NAME);
    return n ? n->text : "";
}

static float dot3(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

bool kit_world_to_screen(Editor* ed, const float p[3], float out[2]) {
    const RenderView* v = &ed->render_view;
    const float* r = ed->vp_rect;
    float d[3] = { p[0] - v->pos[0], p[1] - v->pos[1], p[2] - v->pos[2] };
    float x = dot3(d, v->right), y = dot3(d, v->up), z = dot3(d, v->fwd);
    float cx = r[0] + r[2] * 0.5f, cy = r[1] + r[3] * 0.5f;
    if (v->ortho_half > 0.0f) {
        float s = r[3] * 0.5f / v->ortho_half;
        out[0] = cx + x * s, out[1] = cy - y * s;
        return true;
    }
    if (z <= 1.0f) return false;
    float f = r[2] * 0.5f / tanf((v->fov_deg > 1.0f ? v->fov_deg : 90.0f) * 0.5f * 3.14159265f / 180.0f);
    out[0] = cx + x / z * f, out[1] = cy - y / z * f;
    return true;
}

// A ray against an actor's mesh box (its transform: rotation and scale): the distance and the face's normal (world)
static bool ray_box(const float o[3], const float d[3], const CTransform* t, const float mn[3], const float mx[3], float* out_t, float n[3]) {
    float ax[3][3];
    rotator_axes(t->rotation, ax);
    float lo[3], ld[3], rel[3] = { o[0] - t->location[0], o[1] - t->location[1], o[2] - t->location[2] };
    for (int k = 0; k < 3; k++) { // (into the box's space: the axes, then the scale)
        float s = t->scale[k] != 0.0f ? t->scale[k] : 1e-6f;
        lo[k] = dot3(rel, ax[k]) / s, ld[k] = dot3(d, ax[k]) / s;
    }
    float t0 = 0.0f, t1 = 1e30f;
    int face = -1;
    float sign = 1.0f;
    for (int k = 0; k < 3; k++) {
        if (fabsf(ld[k]) < 1e-9f) {
            if (lo[k] < mn[k] || lo[k] > mx[k]) return false;
            continue;
        }
        float a = (mn[k] - lo[k]) / ld[k], b = (mx[k] - lo[k]) / ld[k];
        float sg = -1.0f;
        if (a > b) { float tmp = a; a = b, b = tmp, sg = 1.0f; }
        if (a > t0) t0 = a, face = k, sign = sg;
        if (b < t1) t1 = b;
        if (t0 > t1) return false;
    }
    if (face < 0) return false; // (the ray starts inside it)
    *out_t = t0;
    for (int k = 0; k < 3; k++) n[k] = ax[face][k] * sign;
    return true;
}

bool kit_ground_below(Editor* ed, const float from[3], Entity ignore, float hit[3], float normal[3]) {
    float o[3] = { from[0], from[1], from[2] + 1.0f }, d[3] = { 0, 0, -1 };
    if (ed->playing) { // (in Play: the scene queries, the play world's collision)
        SqQuery q;
        memset(&q, 0, sizeof q);
        memcpy(q.start, o, sizeof o);
        q.end[0] = o[0], q.end[1] = o[1], q.end[2] = o[2] - 1000000.0f;
        q.shape = SQ_LINE;
        q.nignore = ignore ? 1 : 0;
        q.ignore = &ignore;
        SqHit h;
        if (!sq_trace(&ed->world, &q, &h) || !h.blocking) return false;
        memcpy(hit, h.impact_point, sizeof(float) * 3);
        if (normal) memcpy(normal, h.impact_normal, sizeof(float) * 3);
        return true;
    }
    // the editor: every visible mesh's box (as the viewport's picking) and the Landscape
    World* w = &ed->world;
    float best = 1e30f, bn[3] = { 0, 0, 1 };
    for (uint32_t ai = 0; ai < w->archetype_count; ai++) {
        const EcsArchetype* a = &w->archetypes[ai];
        if (!(a->mask & COMP_BIT(COMP_TRANSFORM)) || !(a->mask & COMP_BIT(COMP_STATIC_MESH))) continue;
        const CTransform* tr = ecs_arch_column(a, COMP_TRANSFORM);
        const CStaticMesh* sm = ecs_arch_column(a, COMP_STATIC_MESH);
        const Entity* ents = ecs_arch_entities(a);
        for (uint32_t i = 0; i < a->count; i++) {
            if (ents[i] == ignore || (tr[i].flags & (TRANSFORM_HIDDEN | TRANSFORM_NO_COLLISION))) continue;
            float mn[3], mx[3], t, n[3];
            render_mesh_bounds(sm[i].mesh, mn, mx);
            if (ray_box(o, d, &tr[i], mn, mx, &t, n) && t < best) best = t, memcpy(bn, n, sizeof bn);
        }
    }
    float lt, ln[3];
    Entity la;
    float far_p[3] = { o[0], o[1], o[2] - 1000000.0f };
    if (landscape_raycast(o, far_p, 0.0f, &lt, ln, &la) && lt * 1000000.0f < best) best = lt * 1000000.0f, memcpy(bn, ln, sizeof bn);
    if (best >= 1e29f) return false;
    hit[0] = o[0], hit[1] = o[1], hit[2] = o[2] - best;
    if (normal) memcpy(normal, bn, sizeof bn);
    return true;
}

Entity kit_spawn_test_cube(Editor* ed, const char* name, float x, float y, float z) {
    Entity e = editor_spawn_shape(ed, SHAPE_CUBE, (float[3]){ x, y, z }, 0.0);
    CName* n = e ? ecs_get(&ed->world, e, COMP_NAME) : NULL;
    if (n && name) snprintf(n->text, sizeof n->text, "%s", name);
    return e;
}

void kit_destroy(Editor* ed, Entity e) {
    if (!ecs_alive(&ed->world, e)) return;
    if (editor_is_selected(ed, e)) editor_select(ed, e, SEL_REMOVE);
    ecs_destroy(&ed->world, e);
}

bool kit_project_file(char* out, size_t cap, const char* rel) {
    char pr[800];
    project_root(pr, sizeof pr);
    if (pr[0]) return snprintf(out, cap, "%s/%s", pr, rel), true;
    char exe[800];
    if (!re_exe_dir(exe, sizeof exe)) exe[0] = '.', exe[1] = 0;
    snprintf(out, cap, "%s/%s", exe, rel);
    return false;
}

// --- the project's assets ---
#define KIT_MAX_ASSETS 8192
static KitAsset g_assets[KIT_MAX_ASSETS]; // (fixed: the kit's index, not a plugin's memory)
static uint32_t g_nassets;
static double g_assets_at = -10.0;
typedef struct { char disk[700]; char vpath[300]; int depth; } WalkCtx;
static void walk_dir(const char* disk, const char* vpath, int depth);
static void walk_one(void* user, const ReDirEntry* de) {
    const WalkCtx* c = user;
    if (de->name[0] == '.' || g_nassets >= KIT_MAX_ASSETS) return;
    char disk[900], vp[600];
    snprintf(disk, sizeof disk, "%s/%s", c->disk, de->name);
    snprintf(vp, sizeof vp, "%s/%s", c->vpath, de->name);
    if (de->is_dir) {
        if (c->depth < 24) walk_dir(disk, vp, c->depth + 1);
        return;
    }
    if (strlen(vp) >= sizeof g_assets[0].vpath) return;
    KitAsset* a = &g_assets[g_nassets++];
    snprintf(a->vpath, sizeof a->vpath, "%s", vp);
    a->hash = bp_vpath_hash(vp);
    a->kind = (int)content_kind_of(de->name);
    a->size = de->size;
}
static void walk_dir(const char* disk, const char* vpath, int depth) {
    WalkCtx c;
    snprintf(c.disk, sizeof c.disk, "%s", disk);
    snprintf(c.vpath, sizeof c.vpath, "%s", vpath);
    c.depth = depth;
    re_dir_walk(disk, walk_one, &c);
}
uint32_t kit_assets(const KitAsset** out, bool refresh) {
    double now = re_time_seconds();
    if ((refresh && now - g_assets_at > 1.0) || g_assets_at < 0.0) {
        g_nassets = 0;
        if (content_root() && content_root()[0]) walk_dir(content_root(), "/Content", 0);
        g_assets_at = now;
    }
    if (out) *out = g_assets;
    return g_nassets;
}
const char* kit_asset_by_hash(uint32_t h) {
    kit_assets(NULL, false);
    for (uint32_t i = 0; i < g_nassets && h; i++)
        if (g_assets[i].hash == h) return g_assets[i].vpath;
    return "";
}
const char* kit_file_name(const char* vpath) {
    static char b[128];
    const char* s = strrchr(vpath, '/');
    snprintf(b, sizeof b, "%s", s ? s + 1 : vpath);
    char* dot = strrchr(b, '.');
    if (dot && dot != b) *dot = 0;
    return b;
}

// --- the self-tests: each plugin on (its window drawn by the frames after), its test, off: none of its memory left ---
int kit_selftest(Editor* ed) {
    kit_init();
    kit_sync(ed);
    int fails = 0;
    double t0 = re_time_seconds();
    for (uint32_t i = 0; i < KIT_N; i++) {
        const KitPlugin* k = g_kit[i];
        bool was = kit_on(i);
        size_t live0 = kit_live_bytes();
        plugin_set_enabled((uint32_t)g_idx[i], true);
        kit_sync(ed);
        bool on = kit_on(i);
        char msg[300] = "";
        double s0 = re_time_seconds();
        bool ok = on && (!k->selftest || k->selftest(ed, msg, sizeof msg));
        double ms = (re_time_seconds() - s0) * 1000.0;
        plugin_set_enabled((uint32_t)g_idx[i], false);
        kit_sync(ed);
        size_t leak = kit_live_bytes() - live0;
        bool off = !kit_on(i);
        printf("[RealEngine] Plugin kit: %-18s %-12s on %s, off %s, leak %zu B, %.2f ms: %s%s%s\n", k->name, k->category, on ? "ok" : "FAILED",
               off ? "ok" : "FAILED", leak, ms, ok && off && !leak ? "ok" : "FAIL", msg[0] ? " - " : "", msg);
        if (!ok || !off || leak) fails++;
        plugin_set_enabled((uint32_t)g_idx[i], was);
        kit_sync(ed);
    }
    printf("[RealEngine] Plugin kit: %u built-in plugins, %d failed, %.1f ms\n", KIT_N, fails, (re_time_seconds() - t0) * 1000.0);
    return fails;
}
