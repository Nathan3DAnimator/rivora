// (2026-11-25, item 18) The kit's Audio, AI, UI and Online plugins (plugin_kit.h): Sound Audit, Sound Map, AI Overview,
// Nav Path Tester, Safe Frames, Net Stats.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "plugin_kit.h"

#include "ai.h"
#include "debug_draw.h"
#include "i18n.h"
#include "navmesh.h"
#include "online.h"
#include "platform.h"
#include "theme.h"
#include "ui_number.h"

static const char* sound_name(uint32_t h) {
    if (!h) return "";
    const char* vp = kit_asset_by_hash(h);
    return vp[0] ? kit_file_name(vp) : "?";
}

// ---------------------------------------------------------------------------------------------------------------
// Sound Audit: every Audio Component of the level: its sound, volume, pitch, how it plays, and what looks wrong
enum { SA_NO_SOUND = 1, SA_MISSING = 2, SA_SILENT = 4, SA_LOUD = 8, SA_FLAT = 16 };
static uint32_t sound_issues(const CAudio* a) {
    uint32_t f = 0;
    if (!a->sound) f |= SA_NO_SOUND;
    else if (!kit_asset_by_hash(a->sound)[0]) f |= SA_MISSING;
    if (a->volume <= 0.0f) f |= SA_SILENT;
    if (a->volume > 2.0f) f |= SA_LOUD;
    if (!a->spatialize && !a->ui) f |= SA_FLAT;
    return f;
}
static void sound_audit_window(Editor* ed, double now) {
    (void)now;
    if (igIsWindowAppearing()) kit_assets(NULL, true);
    World* w = &ed->world;
    uint32_t n = 0, bad = 0;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        const CAudio* a = e ? ecs_get(w, e, COMP_AUDIO) : NULL;
        if (a) n++, bad += sound_issues(a) & (SA_NO_SOUND | SA_MISSING | SA_SILENT | SA_LOUD) ? 1 : 0;
    }
    igText(TR("%u sounds in the level, %u to check"), n, bad);
    if (igBeginTable("sounds", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) {
        igTableSetupScrollFreeze(0, 1);
        igTableSetupColumn(TR("Actor"), 0, 0, 0);
        igTableSetupColumn(TR("Sound"), 0, 0, 0);
        igTableSetupColumn(TR("Volume"), 0, 0, 0);
        igTableSetupColumn(TR("Pitch"), 0, 0, 0);
        igTableSetupColumn(TR("Check"), 0, 0, 0);
        igTableHeadersRow();
        for (uint32_t s = 1; s < w->slot_count; s++) {
            Entity e = ecs_entity_at(w, s);
            const CAudio* a = e ? ecs_get(w, e, COMP_AUDIO) : NULL;
            if (!a) continue;
            uint32_t f = sound_issues(a);
            igTableNextRow(0, 0);
            igTableNextColumn();
            igPushID_Int((int)s);
            if (igSelectable_Bool(kit_actor_name(w, e), editor_is_selected(ed, e), ImGuiSelectableFlags_SpanAllColumns, (ImVec2){ 0, 0 }))
                editor_select(ed, e, SEL_REPLACE), viewport_focus(ed, e);
            igPopID();
            igTableNextColumn(), igTextUnformatted(a->sound ? sound_name(a->sound) : "-", NULL);
            igTableNextColumn(), igText("%.2f", a->volume);
            igTableNextColumn(), igText("%.2f", a->pitch);
            igTableNextColumn();
            if (f & SA_NO_SOUND) igTextColored(THV(TK_ERROR), "%s", TR("no sound"));
            else if (f & SA_MISSING) igTextColored(THV(TK_ERROR), "%s", TR("its sound is missing"));
            else if (f & SA_SILENT) igTextColored(THV(TK_WARN), "%s", TR("silent"));
            else if (f & SA_LOUD) igTextColored(THV(TK_WARN), "%s", TR("very loud"));
            else if (f & SA_FLAT) igTextColored(THV(TK_INFO), "%s", TR("not spatialized"));
            else igTextColored(THV(TK_OK), "%s", TR("ok"));
        }
        igEndTable();
    }
}
static Entity test_sound(Editor* ed, const char* name, float x, float vol) {
    Entity e = kit_spawn_test_cube(ed, name, x, 0, 0);
    if (!e || !ecs_add(&ed->world, e, COMP_AUDIO)) return e;
    CAudio* a = ecs_get(&ed->world, e, COMP_AUDIO);
    if (a) a->volume = vol, a->pitch = 1.0f, a->spatialize = 1;
    return e;
}
static bool sound_audit_test(Editor* ed, char* msg, size_t cap) {
    Entity a = test_sound(ed, "SndQuiet", 0, 0.0f), b = test_sound(ed, "SndLoud", 100, 3.0f);
    CAudio* ca = ecs_get(&ed->world, a, COMP_AUDIO);
    CAudio* cb = ecs_get(&ed->world, b, COMP_AUDIO);
    uint32_t fa = ca ? sound_issues(ca) : 0, fb = cb ? sound_issues(cb) : 0;
    snprintf(msg, cap, "a silent one without a sound and a very loud one flagged");
    kit_destroy(ed, a), kit_destroy(ed, b);
    return (fa & SA_NO_SOUND) && (fa & SA_SILENT) && (fb & SA_LOUD) && !(fb & SA_SILENT);
}

const KitPlugin kit_sound_audit = { "SoundAudit", "Sound Audit", "Audio", "SOUND",
    "Every sound placed in the level in a table (its sound, volume, pitch) and what to check: no sound or a missing one, silent, very loud, not spatialized.",
    false, sound_audit_window, NULL, NULL, NULL, sound_audit_test };

// ---------------------------------------------------------------------------------------------------------------
// Sound Map: the level's sounds as labels over the viewport (the actor and its sound), the silent ones dimmed
static bool g_sm_auto_only;
static uint32_t sound_map_draw(Editor* ed, struct ImDrawList* dl) {
    World* w = &ed->world;
    uint32_t n = 0;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        const CAudio* a = e ? ecs_get(w, e, COMP_AUDIO) : NULL;
        const CTransform* t = a ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
        if (!t || (g_sm_auto_only && !a->auto_activate)) continue;
        char l[160];
        snprintf(l, sizeof l, "%s: %s", kit_actor_name(w, e), a->sound ? sound_name(a->sound) : TR("no sound"));
        if (dl) kit_label(dl, ed, t->location, a->volume > 0.0f && a->sound ? TH(TK_VIOLET_L) : TH(TK_TEXT_DISABLED), l);
        n++;
    }
    return n;
}
static void sound_map_overlay(Editor* ed, struct ImDrawList* dl) {
    if (!ed->game_view) sound_map_draw(ed, dl);
}
static void sound_map_window(Editor* ed, double now) {
    (void)ed, (void)now;
    igTextWrapped("%s", TR("Shows every sound placed in the level over the viewport: the actor and its sound; the silent ones and those without a sound are dimmed."));
    igCheckbox(TR("Only the Auto Activate Ones"), &g_sm_auto_only);
}
static bool sound_map_test(Editor* ed, char* msg, size_t cap) {
    uint32_t before = sound_map_draw(ed, NULL);
    Entity a = test_sound(ed, "SndMapA", 0, 1.0f), b = test_sound(ed, "SndMapB", 100, 1.0f);
    uint32_t n = sound_map_draw(ed, NULL);
    snprintf(msg, cap, "%u sounds labeled", n);
    kit_destroy(ed, a), kit_destroy(ed, b);
    return n == before + 2;
}

const KitPlugin kit_sound_map = { "SoundMap", "Sound Map", "Audio", "SOUND_WAVE",
    "Labels every sound placed in the level in the viewport (the actor and its sound) so you find the ambient sounds, the silent ones dimmed.",
    false, sound_map_window, sound_map_overlay, NULL, NULL, sound_map_test };

// ---------------------------------------------------------------------------------------------------------------
// AI Overview: in Play, what each agent does now (its behavior, whether it sees, its target); in the editor, the level's
// agents and their senses (drawn for the selected ones)
static uint32_t ai_agents(World* w, Entity* out, uint32_t cap) {
    uint32_t n = 0;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        if (e && ecs_get(w, e, COMP_AI_AGENT)) {
            if (out && n < cap) out[n] = e;
            n++;
        }
    }
    return n;
}
static void ai_window(Editor* ed, double now) {
    (void)now;
    igCheckbox(TR("AI Overlay in Play (')"), &ed->show_ai);
    igSameLine(0, -1);
    igCheckbox(TR("Show NavMesh (P)"), &ed->show_nav);
    if (ed->playing) {
        static AiDebug d[64];
        uint32_t n = ai_debug(d, 64);
        const AiStats* st = ai_stats();
        igText(TR("%u agents, %u paths this frame, %.2f ms"), st ? st->agents : n, st ? st->paths_this_frame : 0, st ? st->ms : 0.0);
        for (uint32_t i = 0; i < n && i < 64; i++) {
            igPushID_Int((int)i);
            char l[200];
            snprintf(l, sizeof l, "%s: %s%s%s", kit_actor_name(&ed->world, d[i].pawn), d[i].what ? d[i].what : "-", d[i].sees ? TR(", sees") : "",
                     d[i].has_target ? TR(", has a target") : "");
            if (igSelectable_Bool(l, editor_is_selected(ed, d[i].pawn), 0, (ImVec2){ 0, 0 })) editor_select(ed, d[i].pawn, SEL_REPLACE);
            igPopID();
        }
        return;
    }
    Entity a[128];
    uint32_t n = ai_agents(&ed->world, a, 128);
    igText(TR("%u AI agents in the level (Play shows what they do)"), n);
    for (uint32_t i = 0; i < n && i < 128; i++) {
        const CAIAgent* ag = ecs_get(&ed->world, a[i], COMP_AI_AGENT);
        igPushID_Int((int)i);
        char l[200];
        snprintf(l, sizeof l, TR("%s: sight %.0f m, hearing %.0f m, team %u"), kit_actor_name(&ed->world, a[i]), ag->sight_radius / 100.0f, ag->hearing_radius / 100.0f, ag->team);
        if (igSelectable_Bool(l, editor_is_selected(ed, a[i]), 0, (ImVec2){ 0, 0 })) editor_select(ed, a[i], SEL_REPLACE), viewport_focus(ed, a[i]);
        igPopID();
    }
}
static void ai_tick(Editor* ed, double dt) { // the selected agents' sight and hearing, in the editor
    (void)dt;
    if (ed->playing || ed->game_view) return;
    for (uint32_t i = 0; i < ed->nsel; i++) {
        const CAIAgent* ag = ecs_get(&ed->world, ed->sel[i], COMP_AI_AGENT);
        const CTransform* t = ag ? ecs_get(&ed->world, ed->sel[i], COMP_TRANSFORM) : NULL;
        if (!t) continue;
        if (ag->sight_radius > 0) dd_sphere(t->location, ag->sight_radius, dd_rgba(0.2f, 0.9f, 0.7f, 0.5f), 1.0f, 0.0f);
        if (ag->hearing_radius > 0) dd_sphere(t->location, ag->hearing_radius, dd_rgba(0.6f, 0.5f, 1.0f, 0.4f), 1.0f, 0.0f);
    }
}
static bool ai_test(Editor* ed, char* msg, size_t cap) {
    uint32_t before = ai_agents(&ed->world, NULL, 0);
    Entity e = kit_spawn_test_cube(ed, "AiGuard", 0, 0, 0);
    bool add = e && ecs_add(&ed->world, e, COMP_AI_AGENT);
    uint32_t n = ai_agents(&ed->world, NULL, 0);
    editor_select(ed, e, SEL_REPLACE);
    ai_tick(ed, 0.0);
    snprintf(msg, cap, "%u agents listed (one more), its senses drawn", n);
    kit_destroy(ed, e);
    return add && n == before + 1;
}

const KitPlugin kit_ai_overview = { "AIOverview", "AI Overview", "AI", "BRAIN",
    "The level's AI agents in one list: in the editor their senses (drawn for the selected); in Play what each one does now, whether it sees and has a target. The AI overlay and NavMesh switches.",
    false, ai_window, NULL, ai_tick, NULL, ai_test };

// ---------------------------------------------------------------------------------------------------------------
// Nav Path Tester: the path an agent would take between the two last selected actors, drawn, with its length
#define NP_MAX 256
static float g_np[NP_MAX][3];
static uint32_t g_nnp;
static bool g_np_ok;
static double g_np_at;

static float path_length(const float from[3], const float (*p)[3], uint32_t n) {
    float len = 0;
    const float* a = from;
    for (uint32_t i = 0; i < n; i++) {
        len += sqrtf((p[i][0] - a[0]) * (p[i][0] - a[0]) + (p[i][1] - a[1]) * (p[i][1] - a[1]) + (p[i][2] - a[2]) * (p[i][2] - a[2]));
        a = p[i];
    }
    return len;
}
static bool np_ends(Editor* ed, float a[3], float b[3]) {
    if (ed->nsel < 2) return false;
    const CTransform* ta = ecs_get(&ed->world, ed->sel[ed->nsel - 2], COMP_TRANSFORM);
    const CTransform* tb = ecs_get(&ed->world, ed->sel[ed->nsel - 1], COMP_TRANSFORM);
    if (!ta || !tb) return false;
    memcpy(a, ta->location, sizeof(float) * 3), memcpy(b, tb->location, sizeof(float) * 3);
    return true;
}
static void np_tick(Editor* ed, double dt) { // the path asked again four times a second, drawn every frame
    (void)dt;
    float a[3], b[3];
    if (!np_ends(ed, a, b)) {
        g_nnp = 0, g_np_ok = false;
        return;
    }
    if (re_time_seconds() - g_np_at > 0.25) {
        g_np_at = re_time_seconds();
        g_nnp = 0;
        g_np_ok = navmesh_ready() ? navmesh_path(a, b, g_np, NULL, NP_MAX, &g_nnp) : ai_nav_path(a, b, g_np, NP_MAX, &g_nnp);
    }
    if (!g_np_ok || ed->game_view) return;
    const float* prev = a;
    for (uint32_t i = 0; i < g_nnp; i++) {
        dd_line(prev, g_np[i], dd_rgba(0.2f, 0.85f, 1.0f, 0.95f), 3.0f, 0.0f);
        dd_point(g_np[i], dd_rgba(1.0f, 1.0f, 1.0f, 1.0f), 6.0f, 0.0f);
        prev = g_np[i];
    }
}
static void np_window(Editor* ed, double now) {
    (void)now;
    const NavMeshStats* st = navmesh_stats();
    if (navmesh_ready() && st)
        igText(TR("NavMesh: %u tiles, %u polygons, %u links"), st->tiles, st->polys, st->links);
    else igTextColored(THV(TK_INFO), "%s", TR("No NavMesh built: the path uses the AI's grid. Add a Nav Mesh Bounds Volume to build one."));
    float a[3], b[3];
    if (!np_ends(ed, a, b)) {
        igTextDisabled("%s", TR("Select two actors: the path goes from the first to the second."));
        return;
    }
    if (g_np_ok) igText(TR("Path: %u points, %.1f m (straight line %.1f m)"), g_nnp, path_length(a, (const float(*)[3])g_np, g_nnp) / 100.0f,
                        sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2])) / 100.0f);
    else igTextColored(THV(TK_WARN), "%s", TR("No path between them."));
}
static bool np_test(Editor* ed, char* msg, size_t cap) {
    (void)ed;
    float from[3] = { 0, 0, 0 }, pts[3][3] = { { 300, 0, 0 }, { 300, 400, 0 }, { 300, 400, 120 } };
    float len = path_length(from, (const float(*)[3])pts, 3);
    float a[3] = { 0, 0, 0 }, b[3] = { 100, 0, 0 }, out[8][3];
    uint32_t n = 0;
    bool asked = true; // (a request with or without a NavMesh answers, never crashes)
    if (navmesh_ready()) navmesh_path(a, b, out, NULL, 8, &n);
    else ai_nav_path(a, b, out, 8, &n);
    snprintf(msg, cap, "a 3-point path measured %.0f cm; a request answered with %u points (%s)", len, n, navmesh_ready() ? "NavMesh" : "grid");
    return asked && fabsf(len - 820.0f) < 0.01f;
}
static void np_off(Editor* ed) { (void)ed, g_nnp = 0, g_np_ok = false; }

const KitPlugin kit_nav_tester = { "NavPathTester", "Nav Path Tester", "AI", "NAVMESH",
    "The path an AI would walk between the two last selected actors, drawn in the viewport with its length against the straight line; the NavMesh's tiles and polygons.",
    false, np_window, NULL, np_tick, np_off, np_test };

// ---------------------------------------------------------------------------------------------------------------
// Safe Frames: a screen's shape over the viewport (16:9, 21:9, 4:3, a phone's 9:16, VR's square) with the outside dimmed,
// the title safe (90%) and action safe (95%) areas and the rule of thirds
static int g_sf_ratio;
static bool g_sf_title = true, g_sf_action = true, g_sf_thirds;
static const float g_sf_ratios[] = { 16.0f / 9.0f, 21.0f / 9.0f, 4.0f / 3.0f, 9.0f / 16.0f, 1.0f, 19.5f / 9.0f };
static const char* const g_sf_names[] = { "16:9 (TV, PC)", "21:9 (ultrawide)", "4:3", "9:16 (phone, portrait)", "1:1 (VR)", "19.5:9 (phone, landscape)" };

static void sf_rect(const float vp[4], float ratio, float out[4]) { // the largest rect of that shape centered in vp
    float w = vp[2], h = vp[2] / ratio;
    if (h > vp[3]) h = vp[3], w = vp[3] * ratio;
    out[0] = vp[0] + (vp[2] - w) * 0.5f, out[1] = vp[1] + (vp[3] - h) * 0.5f, out[2] = w, out[3] = h;
}
static void sf_overlay(Editor* ed, struct ImDrawList* dl) {
    float r[4];
    const float* v = ed->vp_rect;
    sf_rect(v, g_sf_ratios[g_sf_ratio], r);
    uint32_t dim = THA(TK_BG_DEEP, 150), line = THA(TK_TEXT, 150);
    ImDrawList_AddRectFilled(dl, (ImVec2){ v[0], v[1] }, (ImVec2){ v[0] + v[2], r[1] }, dim, 0, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2){ v[0], r[1] + r[3] }, (ImVec2){ v[0] + v[2], v[1] + v[3] }, dim, 0, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2){ v[0], r[1] }, (ImVec2){ r[0], r[1] + r[3] }, dim, 0, 0);
    ImDrawList_AddRectFilled(dl, (ImVec2){ r[0] + r[2], r[1] }, (ImVec2){ v[0] + v[2], r[1] + r[3] }, dim, 0, 0);
    ImDrawList_AddRect(dl, (ImVec2){ r[0], r[1] }, (ImVec2){ r[0] + r[2], r[1] + r[3] }, line, 0, 1.0f, 0);
    for (int k = 0; k < 2; k++) {
        if (!(k ? g_sf_title : g_sf_action)) continue;
        float m = k ? 0.05f : 0.025f; // (title 90%, action 95%)
        ImDrawList_AddRect(dl, (ImVec2){ r[0] + r[2] * m, r[1] + r[3] * m }, (ImVec2){ r[0] + r[2] * (1 - m), r[1] + r[3] * (1 - m) },
                           k ? THA(TK_AMBER, 170) : THA(TK_GREEN, 170), 0, 1.0f, 0);
    }
    if (g_sf_thirds)
        for (int i = 1; i < 3; i++) {
            ImDrawList_AddLine(dl, (ImVec2){ r[0] + r[2] * (float)i / 3.0f, r[1] }, (ImVec2){ r[0] + r[2] * (float)i / 3.0f, r[1] + r[3] }, THA(TK_TEXT, 70), 1.0f);
            ImDrawList_AddLine(dl, (ImVec2){ r[0], r[1] + r[3] * (float)i / 3.0f }, (ImVec2){ r[0] + r[2], r[1] + r[3] * (float)i / 3.0f }, THA(TK_TEXT, 70), 1.0f);
        }
}
static void sf_window(Editor* ed, double now) {
    (void)ed, (void)now;
    igTextWrapped("%s", TR("Shows a screen's shape over the viewport with the rest dimmed, and the safe areas where the HUD and the important things must stay on a TV."));
    const char* shown[6];
    for (int i = 0; i < 6; i++) shown[i] = TR(g_sf_names[i]);
    igCombo_Str_arr(TR("Screen Shape"), &g_sf_ratio, shown, 6, -1);
    igCheckbox(TR("Title Safe (90%)"), &g_sf_title);
    igCheckbox(TR("Action Safe (95%)"), &g_sf_action);
    igCheckbox(TR("Rule of Thirds"), &g_sf_thirds);
}
static bool sf_test(Editor* ed, char* msg, size_t cap) {
    (void)ed;
    float r[4], p[4];
    sf_rect((float[4]){ 0, 0, 1000, 1000 }, 16.0f / 9.0f, r);
    sf_rect((float[4]){ 0, 0, 1600, 900 }, 9.0f / 16.0f, p);
    snprintf(msg, cap, "16:9 in 1000 x 1000: %.0f x %.1f; 9:16 in 1600 x 900: %.1f x %.0f", r[2], r[3], p[2], p[3]);
    return r[2] == 1000.0f && fabsf(r[3] - 562.5f) < 0.01f && fabsf(r[1] - 218.75f) < 0.01f && fabsf(p[2] - 506.25f) < 0.01f && p[3] == 900.0f;
}

const KitPlugin kit_safe_frames = { "SafeFrames", "Safe Frames", "UI", "WIDGET",
    "A screen's shape over the viewport (16:9, 21:9, 4:3, phone, VR) with the outside dimmed, the TV's title and action safe areas and the rule of thirds: lay out the HUD and frame the shots.",
    false, sf_window, sf_overlay, NULL, NULL, sf_test };

// ---------------------------------------------------------------------------------------------------------------
// Net Stats: the network's numbers over the viewport while online (ping, jitter, loss, the traffic, corrections), and
// each player's ping and loss in the window
static void net_text(const OnlineStats* s, char* out, size_t cap) {
    snprintf(out, cap, "%s %d  %s %.0f ms (+-%.0f)\n%s %.1f%%  %s %.1f / %.1f kbps\n%s %u  %s %u", TR("Peers"), s->peers, TR("Ping"), s->rtt_ms, s->jitter_ms,
             TR("Loss"), s->loss * 100.0f, TR("Out / In"), s->kbps_out, s->kbps_in, TR("Replicated"), (unsigned)s->replicated, TR("Corrections"), s->corrections);
}
static void net_overlay(Editor* ed, struct ImDrawList* dl) {
    OnlineStats s = online_stats();
    if (!s.peers && !ed->playing) return;
    char t[400];
    net_text(&s, t, sizeof t);
    kit_hud(dl, ed, s.loss > 0.05f || s.rtt_ms > 150.0f ? TK_WARN : TK_LIME, TR("Net Stats"), t);
}
static void net_window(Editor* ed, double now) {
    (void)ed, (void)now;
    OnlineStats s = online_stats();
    char t[400];
    net_text(&s, t, sizeof t);
    igTextUnformatted(t, NULL);
    igSeparatorText(TR("Players"));
    int n = online_player_count();
    for (int p = 0; p < n; p++) igText(TR("%s: ping %.0f ms, loss %.1f%%"), online_player_name(p), online_player_ping(p), online_player_loss(p) * 100.0f);
    if (!n) igTextDisabled("%s", TR("Not in an online session (Play with 2 players or more, or a LAN / Steam session)."));
}
static bool net_test(Editor* ed, char* msg, size_t cap) {
    (void)ed;
    OnlineStats s;
    memset(&s, 0, sizeof s);
    s.peers = 3, s.rtt_ms = 48.0f, s.loss = 0.02f, s.kbps_out = 12.5f;
    char t[400];
    net_text(&s, t, sizeof t);
    OnlineStats live = online_stats(); // (offline: zeros, no crash)
    snprintf(msg, cap, "the panel reads \"%.*s...\"; offline: %d peers", 22, t, live.peers);
    return strstr(t, "48 ms") && strstr(t, "2.0%") && strstr(t, "12.5");
}

const KitPlugin kit_net_stats = { "NetStats", "Net Stats", "Online", "SIGNAL",
    "The network's numbers over the viewport while online (peers, ping and jitter, loss, the traffic out and in, corrections) and each player's ping and loss.",
    false, net_window, net_overlay, NULL, NULL, net_test };
