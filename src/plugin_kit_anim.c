// (2026-11-25, item 18) The kit's Animation plugins (plugin_kit.h): Animation Retime, Animation Audit, Motion Trails.
#define CIMGUI_DEFINE_ENUMS_AND_STRUCTS
#include <cimgui.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "plugin_kit.h"

#include "i18n.h"
#include "platform.h"
#include "theme.h"
#include "ui_number.h"

// ---------------------------------------------------------------------------------------------------------------
// Animation Retime: the selected Skeletal Meshes' play rate set or scaled, with a random spread (a crowd out of step)
static float g_rate = 1.0f, g_spread = 0.0f;
static bool g_rate_mul;
static int g_rate_loop = -1; // -1 kept, 0 off, 1 on

static uint32_t retime_apply(Editor* ed, float rate, bool multiply, float spread, int loop) {
    uint32_t n = 0, s = 0x1234567u;
    bool pushed = false;
    for (uint32_t i = 0; i < ed->nsel; i++) {
        CSkeletalMesh* sk = ecs_get(&ed->world, ed->sel[i], COMP_SKELETAL_MESH);
        if (!sk) continue;
        if (!pushed) editor_undo_push(ed), pushed = true;
        sk = ecs_get(&ed->world, ed->sel[i], COMP_SKELETAL_MESH);
        s ^= s << 13, s ^= s >> 17, s ^= s << 5;
        float jitter = 1.0f + spread * (((float)(s >> 8) / 16777216.0f) * 2.0f - 1.0f);
        sk->play_rate = (multiply ? sk->play_rate * rate : rate) * jitter;
        if (loop >= 0) sk->loop = (uint32_t)loop;
        ecs_touch(&ed->world, ed->sel[i]);
        n++;
    }
    if (n) ed->dirty = true;
    return n;
}
static void retime_window(Editor* ed, double now) {
    igTextWrapped("%s", TR("Sets the play rate of the selected Skeletal Mesh actors at once, or scales it; a spread puts a crowd out of step."));
    igCheckbox(TR("Multiply the Current Rate"), &g_rate_mul);
    ui_num_float(g_rate_mul ? TR("Multiplier") : TR("Play Rate"), &g_rate, 0.01f, 0.0f, 4.0f, -100.0f, 100.0f, "%.2f", 0);
    ui_num_float(TR("Random Spread"), &g_spread, 0.005f, 0.0f, 0.5f, 0.0f, 1.0f, "%.0f%%", 0);
    static const char* const loops[] = { "Keep", "Off", "On" };
    const char* shown[3] = { TR(loops[0]), TR(loops[1]), TR(loops[2]) };
    int l = g_rate_loop + 1;
    if (igCombo_Str_arr(TR("Loop"), &l, shown, 3, -1)) g_rate_loop = l - 1;
    uint32_t nsk = 0;
    for (uint32_t i = 0; i < ed->nsel; i++) nsk += ecs_get(&ed->world, ed->sel[i], COMP_SKELETAL_MESH) != NULL;
    igText(TR("%u Skeletal Mesh actors selected"), nsk);
    igBeginDisabled(!nsk || ed->playing);
    if (igButton(TR("Apply to Selected"), (ImVec2){ -1, 0 }))
        editor_status(ed, now, TR("Play rate set on %u actors."), retime_apply(ed, g_rate, g_rate_mul, g_spread, g_rate_loop));
    igEndDisabled();
}
static Entity test_skel(Editor* ed, const char* name, float x) {
    Entity e = kit_spawn_test_cube(ed, name, x, 0, 0);
    if (e && ecs_add(&ed->world, e, COMP_SKELETAL_MESH)) {
        CSkeletalMesh* sk = ecs_get(&ed->world, e, COMP_SKELETAL_MESH);
        if (sk) sk->play_rate = 1.0f;
    }
    return e;
}
static bool retime_test(Editor* ed, char* msg, size_t cap) {
    Entity a = test_skel(ed, "RetimeA", 0), b = test_skel(ed, "RetimeB", 100);
    editor_select(ed, a, SEL_REPLACE), editor_select(ed, b, SEL_ADD);
    uint32_t n = retime_apply(ed, 0.5f, false, 0.0f, 1);
    retime_apply(ed, 3.0f, true, 0.0f, -1);
    CSkeletalMesh* sa = ecs_get(&ed->world, a, COMP_SKELETAL_MESH);
    bool ok = n == 2 && sa && fabsf(sa->play_rate - 1.5f) < 1e-4f && sa->loop == 1;
    retime_apply(ed, 1.0f, false, 0.2f, -1);
    CSkeletalMesh* sb = ecs_get(&ed->world, b, COMP_SKELETAL_MESH);
    sa = ecs_get(&ed->world, a, COMP_SKELETAL_MESH);
    bool spread = sa && sb && sa->play_rate != sb->play_rate && fabsf(sa->play_rate - 1.0f) <= 0.2001f;
    snprintf(msg, cap, "0.5 x 3 = %.2f, spread %.2f / %.2f", 1.5f, sa ? sa->play_rate : 0, sb ? sb->play_rate : 0);
    kit_destroy(ed, a), kit_destroy(ed, b);
    return ok && spread;
}

const KitPlugin kit_anim_rate = { "AnimRetime", "Animation Retime", "Animation", "TIMER",
    "Sets or scales the play rate of every selected Skeletal Mesh at once, with a random spread so a crowd doesn't walk in step, and their loop. Undoable.",
    false, retime_window, NULL, NULL, NULL, retime_test };

// ---------------------------------------------------------------------------------------------------------------
// Animation Audit: every Skeletal Mesh actor, what it plays and what it lacks (no animation, a stopped rate, no physics)
typedef struct SkRow { Entity e; uint32_t mesh, anim, anim_class, physics, ik; float rate; } SkRow;
static SkRow* g_sk;
static uint32_t g_nsk, g_cap_sk;

static uint32_t anim_audit(Editor* ed, uint32_t* problems) {
    World* w = &ed->world;
    g_nsk = 0;
    uint32_t bad = 0;
    for (uint32_t s = 1; s < w->slot_count; s++) {
        Entity e = ecs_entity_at(w, s);
        const CSkeletalMesh* sk = e ? ecs_get(w, e, COMP_SKELETAL_MESH) : NULL;
        if (!sk) continue;
        if (g_nsk == g_cap_sk) {
            uint32_t nc = g_cap_sk ? g_cap_sk * 2 : 32;
            SkRow* r = kit_realloc(g_sk, sizeof(SkRow) * g_cap_sk, sizeof(SkRow) * nc);
            if (!r) break;
            g_sk = r, g_cap_sk = nc;
        }
        g_sk[g_nsk++] = (SkRow){ e, sk->mesh, sk->anim, sk->anim_class, sk->physics_asset, sk->ik_rig, sk->play_rate };
        bad += (!sk->anim && !sk->anim_class) || sk->play_rate == 0.0f || !sk->mesh;
    }
    if (problems) *problems = bad;
    return g_nsk;
}
static const char* asset_or(uint32_t h, const char* none) {
    if (!h) return none;
    const char* vp = kit_asset_by_hash(h);
    return vp[0] ? kit_file_name(vp) : "?";
}
static void anim_audit_window(Editor* ed, double now) {
    (void)now;
    static double at;
    uint32_t bad = 0;
    if (igIsWindowAppearing()) kit_assets(NULL, true);
    if (igIsWindowAppearing() || re_time_seconds() - at > 0.5) anim_audit(ed, &bad), at = re_time_seconds();
    for (uint32_t i = 0; i < g_nsk; i++) bad += (!g_sk[i].anim && !g_sk[i].anim_class) || g_sk[i].rate == 0.0f || !g_sk[i].mesh;
    igText(TR("%u Skeletal Mesh actors, %u with a problem"), g_nsk, bad);
    if (igBeginTable("skel", 5, ImGuiTableFlags_RowBg | ImGuiTableFlags_Borders | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable, (ImVec2){ 0, 0 }, 0)) {
        igTableSetupScrollFreeze(0, 1);
        igTableSetupColumn(TR("Actor"), 0, 0, 0);
        igTableSetupColumn(TR("Mesh"), 0, 0, 0);
        igTableSetupColumn(TR("Plays"), 0, 0, 0);
        igTableSetupColumn(TR("Rate"), 0, 0, 0);
        igTableSetupColumn(TR("Physics / IK"), 0, 0, 0);
        igTableHeadersRow();
        for (uint32_t i = 0; i < g_nsk; i++) {
            const SkRow* r = &g_sk[i];
            if (!ecs_alive(&ed->world, r->e)) continue;
            igTableNextRow(0, 0);
            igTableNextColumn();
            igPushID_Int((int)i);
            if (igSelectable_Bool(kit_actor_name(&ed->world, r->e), editor_is_selected(ed, r->e), ImGuiSelectableFlags_SpanAllColumns, (ImVec2){ 0, 0 }))
                editor_select(ed, r->e, SEL_REPLACE), viewport_focus(ed, r->e);
            igPopID();
            igTableNextColumn();
            r->mesh ? igTextUnformatted(asset_or(r->mesh, ""), NULL) : igTextColored(THV(TK_ERROR), "%s", TR("none"));
            igTableNextColumn();
            if (r->anim_class) igText("%s (%s)", asset_or(r->anim_class, ""), TR("Anim Blueprint"));
            else if (r->anim) igTextUnformatted(asset_or(r->anim, ""), NULL);
            else igTextColored(THV(TK_WARN), "%s", TR("no animation"));
            igTableNextColumn();
            r->rate == 0.0f ? igTextColored(THV(TK_WARN), "0 (%s)", TR("stopped")) : igText("%.2f", r->rate);
            igTableNextColumn();
            igText("%s / %s", r->physics ? TR("yes") : "-", r->ik ? TR("yes") : "-");
        }
        igEndTable();
    }
}
static bool anim_audit_test(Editor* ed, char* msg, size_t cap) {
    Entity a = test_skel(ed, "AuditSkA", 0), b = test_skel(ed, "AuditSkB", 100);
    CSkeletalMesh* sb = ecs_get(&ed->world, b, COMP_SKELETAL_MESH);
    if (sb) sb->anim = 0xAB, sb->mesh = 0xCD, sb->play_rate = 1.0f;
    uint32_t bad = 0, before_bad = 0;
    anim_audit(ed, &bad);
    bool found_a = false, found_b = false;
    for (uint32_t i = 0; i < g_nsk; i++) found_a |= g_sk[i].e == a, found_b |= g_sk[i].e == b;
    kit_destroy(ed, a);
    anim_audit(ed, &before_bad);
    snprintf(msg, cap, "%u Skeletal Mesh actors, the one without animation flagged", g_nsk + 1);
    kit_destroy(ed, b);
    return found_a && found_b && bad == before_bad + 1;
}
static void anim_audit_off(Editor* ed) {
    (void)ed;
    kit_free(g_sk, sizeof(SkRow) * g_cap_sk);
    g_sk = NULL, g_nsk = g_cap_sk = 0;
}

const KitPlugin kit_anim_audit = { "AnimAudit", "Animation Audit", "Animation", "SKELETAL_MESH",
    "Every Skeletal Mesh actor in a table: its mesh, what it plays (animation or Anim Blueprint), its rate, its Physics Asset and IK Rig; the ones with no animation or stopped stand out.",
    false, anim_audit_window, NULL, NULL, anim_audit_off, anim_audit_test };

// ---------------------------------------------------------------------------------------------------------------
// Motion Trails: the path the selected actors (and, in Play, every Character) took, drawn as a fading line with its speed
#define TRAIL_ACTORS 16
#define TRAIL_SAMPLES 256
typedef struct Trail { Entity e; uint32_t head, n; float p[TRAIL_SAMPLES][3]; float t[TRAIL_SAMPLES]; } Trail;
static Trail* g_tr; // (TRAIL_ACTORS of them, made when the plugin first records)
static float g_tr_seconds = 4.0f, g_tr_clock;
static bool g_tr_characters = true;

static Trail* trail_for(Entity e) {
    if (!g_tr && !(g_tr = kit_alloc(sizeof(Trail) * TRAIL_ACTORS))) return NULL;
    Trail* free_one = NULL;
    for (int i = 0; i < TRAIL_ACTORS; i++) {
        if (g_tr[i].e == e) return &g_tr[i];
        if (!g_tr[i].e && !free_one) free_one = &g_tr[i];
    }
    if (free_one) memset(free_one, 0, sizeof *free_one), free_one->e = e;
    return free_one;
}
static void trail_record(Editor* ed, Entity e) {
    const CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    Trail* tr = t ? trail_for(e) : NULL;
    if (!tr) return;
    uint32_t last = (tr->head + TRAIL_SAMPLES - 1) % TRAIL_SAMPLES;
    float dx = t->location[0] - tr->p[last][0], dy = t->location[1] - tr->p[last][1], dz = t->location[2] - tr->p[last][2];
    if (tr->n && dx * dx + dy * dy + dz * dz < 4.0f) { tr->t[last] = g_tr_clock; return; } // (still: the last sample's time moves on)
    memcpy(tr->p[tr->head], t->location, sizeof(float) * 3);
    tr->t[tr->head] = g_tr_clock;
    tr->head = (tr->head + 1) % TRAIL_SAMPLES;
    if (tr->n < TRAIL_SAMPLES) tr->n++;
}
static void trails_tick(Editor* ed, double dt) {
    g_tr_clock += (float)dt;
    for (uint32_t i = 0; i < ed->nsel && i < TRAIL_ACTORS; i++) trail_record(ed, ed->sel[i]);
    if (ed->playing && g_tr_characters) {
        World* w = &ed->world;
        int k = 0;
        for (uint32_t s = 1; s < w->slot_count && k < TRAIL_ACTORS; s++) {
            Entity e = ecs_entity_at(w, s);
            if (e && ecs_get(w, e, COMP_CHARACTER)) trail_record(ed, e), k++;
        }
    }
    for (int i = 0; g_tr && i < TRAIL_ACTORS; i++) // (a trail whose actor is gone, or idle past the length, goes)
        if (g_tr[i].e && (!ecs_alive(&ed->world, g_tr[i].e) || g_tr_clock - g_tr[i].t[(g_tr[i].head + TRAIL_SAMPLES - 1) % TRAIL_SAMPLES] > g_tr_seconds * 2.0f))
            g_tr[i].e = ENTITY_NONE, g_tr[i].n = 0;
}
static float trail_length(const Trail* tr, float* speed) {
    float len = 0, recent = 0;
    for (uint32_t k = 1; k < tr->n; k++) {
        uint32_t a = (tr->head + TRAIL_SAMPLES - tr->n + k - 1) % TRAIL_SAMPLES, b = (a + 1) % TRAIL_SAMPLES;
        float d = sqrtf((tr->p[b][0] - tr->p[a][0]) * (tr->p[b][0] - tr->p[a][0]) + (tr->p[b][1] - tr->p[a][1]) * (tr->p[b][1] - tr->p[a][1]) +
                        (tr->p[b][2] - tr->p[a][2]) * (tr->p[b][2] - tr->p[a][2]));
        len += d;
        if (tr->t[b] > g_tr_clock - 0.5f) recent += d;
    }
    if (speed) *speed = recent / 0.5f;
    return len;
}
static void trails_overlay(Editor* ed, struct ImDrawList* dl) {
    for (int i = 0; g_tr && i < TRAIL_ACTORS; i++) {
        const Trail* tr = &g_tr[i];
        if (!tr->e || tr->n < 2) continue;
        float prev[2];
        bool have = false;
        for (uint32_t k = 0; k < tr->n; k++) {
            uint32_t a = (tr->head + TRAIL_SAMPLES - tr->n + k) % TRAIL_SAMPLES;
            float age = g_tr_clock - tr->t[a];
            float s[2];
            if (age > g_tr_seconds || !kit_world_to_screen(ed, tr->p[a], s)) { have = false; continue; }
            if (have) ImDrawList_AddLine(dl, (ImVec2){ prev[0], prev[1] }, (ImVec2){ s[0], s[1] }, THA(TK_PINK, (unsigned)(255.0f * (1.0f - age / g_tr_seconds))), 2.0f);
            prev[0] = s[0], prev[1] = s[1], have = true;
        }
        float sp;
        trail_length(tr, &sp);
        char t[48];
        snprintf(t, sizeof t, "%.0f cm/s", sp);
        const CTransform* x = ecs_get(&ed->world, tr->e, COMP_TRANSFORM);
        if (x && sp > 1.0f) kit_label(dl, ed, x->location, TH(TK_PINK_L), t);
    }
}
static void trails_window(Editor* ed, double now) {
    (void)now;
    igTextWrapped("%s", TR("Draws the path the selected actors take (and every Character in Play) as a fading line, with their speed: root motion, AI paths, a gizmo drag."));
    ui_num_float(TR("Trail Length"), &g_tr_seconds, 0.05f, 0.5f, 30.0f, 0.1f, 600.0f, "%.1f s", 0);
    igCheckbox(TR("Every Character in Play"), &g_tr_characters);
    uint32_t n = 0;
    for (int i = 0; g_tr && i < TRAIL_ACTORS; i++)
        if (g_tr[i].e && g_tr[i].n > 1) {
            float sp, len = trail_length(&g_tr[i], &sp);
            igText("%s: %.1f m, %.0f cm/s", kit_actor_name(&ed->world, g_tr[i].e), len / 100.0f, sp), n++;
        }
    if (!n) igTextDisabled("%s", TR("Nothing moved yet."));
    if (igButton(TR("Clear Trails"), (ImVec2){ 0, 0 }) && g_tr) memset(g_tr, 0, sizeof(Trail) * TRAIL_ACTORS);
}
static void trails_off(Editor* ed) {
    (void)ed;
    kit_free(g_tr, sizeof(Trail) * TRAIL_ACTORS);
    g_tr = NULL;
}
static bool trails_test(Editor* ed, char* msg, size_t cap) {
    Entity e = kit_spawn_test_cube(ed, "Mover", 0, 0, 0);
    editor_select(ed, e, SEL_REPLACE);
    for (int f = 0; f <= 60; f++) { // 1 s at 60 fps, 300 cm/s along X
        CTransform* t = ecs_get(&ed->world, e, COMP_TRANSFORM);
        t->location[0] = 5.0f * (float)f;
        trails_tick(ed, 1.0 / 60.0);
    }
    Trail* tr = trail_for(e);
    float sp = 0, len = tr ? trail_length(tr, &sp) : 0;
    snprintf(msg, cap, "a cube moved 3 m in 1 s: trail %.0f cm over %u samples, %.0f cm/s", len, tr ? tr->n : 0, sp);
    kit_destroy(ed, e);
    trails_tick(ed, 0.0);
    return tr && fabsf(len - 300.0f) < 1.0f && fabsf(sp - 300.0f) < 20.0f;
}

const KitPlugin kit_motion_trails = { "MotionTrails", "Motion Trails", "Animation", "SPLINE",
    "Draws the path the selected actors take, and every Character's in Play, as a fading line with the speed: check root motion, foot sliding, AI paths.",
    false, trails_window, trails_overlay, trails_tick, trails_off, trails_test };
