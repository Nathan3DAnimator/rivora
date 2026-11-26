// (2026-11-25, item 16) --groom-skin-test, after --import of tests/data/import/hair_skinned.tlend (an armature, its Spin
// action turning the Arm bone, the Piece riding it, a hair on the Piece) and --play-eject: the Tlender's fur on the
// Skeletal Mesh's actor (not an actor of its own), its strands bent by the skin's bones (GroomStats.skinned), its root's
// bone turning between two moments (the palette differs), Visible off: no hair drawn, on again: drawn
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "anim.h"
#include "components.h"
#include "ecs.h"
#include "editor.h"
#include "groom.h"
#include "render.h"
#include "vkctx.h"
#include "blueprint.h"
#include "content.h"
#include "platform.h"
#include "platform_files.h"
#include "script.h"

static struct {
    uint32_t frame;
    Entity e;
    bool on_sk, skinned_seen, off_empty, on_again;
    float pal_a[64][3][4], pal_b[64][3][4]; // (2026-11-25: its Skeletal Mesh's bones, up to 64, at two moments)
    uint32_t nb_a, nb_b;
    bool have_a, have_b;
    uint32_t mask_on, mask_off; // (item 17 part 2) the frames its hair went into the selection's mask: on, off
    // (2026-11-25) a class written in Real turning its own Groom's Simulate and Visible on and off (the Blueprints' nodes)
    Entity bp;
    bool bp_sim_on, bp_off, bp_back, bp_simulated;
} T;

static const char* g_toggle_src =
    "class GroomToggle extends Actor\n"
    "    var ticks = 0\n"
    "\n"
    "    event Tick(dt: float)\n"
    "        ticks += 1\n"
    "        if ticks == 10\n"
    "            SetGroomSimulate(true)\n"
    "        if ticks == 50\n"
    "            if GetGroomSimulate() and GetGroomVisible()\n"
    "                SetGroomVisible(false)\n"
    "        if ticks == 70\n"
    "            SetGroomVisible(true)\n"
    "            SetGroomSimulate(false)\n";

static void spawn_toggle(Editor* ed) {
    char dir[900], path[1000];
    snprintf(dir, sizeof dir, "%s/GroomTest", content_root());
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s/GroomToggle.real", dir);
    FILE* fp = re_fopen(path, "wb");
    if (!fp) return;
    fwrite(g_toggle_src, 1, strlen(g_toggle_src), fp);
    fclose(fp);
    ScriptFile* sf = script_load("/Content/GroomTest/GroomToggle.real");
    if (!sf || !sf->ok || !sf->cls) {
        printf("[RealEngine] FAIL: groom skin: the toggling class didn't compile\n");
        for (uint32_t i = 0; sf && i < sf->ndiags; i++) printf("[RealEngine]   (%d:%d): %s\n", sf->diags[i].line, sf->diags[i].col, sf->diags[i].text);
        return;
    }
    T.bp = bp_spawn(&ed->world, sf->cls, (float[3]){ 300.0f, 0.0f, 150.0f }, (float[3]){ 0, 0, 0 });
    if (T.bp && ecs_add(&ed->world, T.bp, COMP_GROOM)) {
        CGroom* g = ecs_get(&ed->world, T.bp, COMP_GROOM);
        ecs_component_defaults(COMP_GROOM, g);
        g->strands = 2000;
    }
}

static bool head_palette(World* w, float out[64][3][4], uint32_t* n) {
    Entity sk = anim_mesh_entity(w, T.e);
    uint32_t nb = 0;
    const float(*pal)[3][4] = sk ? anim_skin_palette(sk, &nb, NULL) : NULL;
    if (!pal || !nb) return false;
    *n = nb < 64 ? nb : 64;
    memcpy(out, pal, sizeof(float) * 12 * *n); // (every bone: the test passes when the animation moves any of them)
    return true;
}

void groom_skin_test_tick(Editor* ed, VkCtx* vk) {
    World* w = &ed->world;
    uint32_t f = T.frame++;
    if (!T.e)
        for (uint32_t s = 1; s < w->slot_count && !T.e; s++) {
            Entity e = ecs_entity_at(w, s);
            if (e && ecs_get(w, e, COMP_GROOM)) T.e = e, T.on_sk = ecs_get(w, e, COMP_SKELETAL_MESH) != NULL;
        }
    if (f == 60) spawn_toggle(ed); // (in the play: its Tick runs)
    if (T.bp && ecs_alive(w, T.bp)) { // the class's steps (its ticks from frame 61): simulated, off, back static and on
        const CGroom* bg = ecs_get(w, T.bp, COMP_GROOM);
        const GroomStats* gs = groom_stats();
        if (bg && f > 80 && f < 105 && bg->dynamic) T.bp_sim_on = true;
        if (bg && f > 80 && f < 105 && gs->simulated) T.bp_simulated = true;
        if (bg && f > 118 && f < 128 && !bg->visible) T.bp_off = true;
        if (bg && f > 140 && bg->visible && !bg->dynamic) T.bp_back = true;
    }
    if (!T.e) return;
    CGroom* g = ecs_get(w, T.e, COMP_GROOM);
    if (!g) return;
    const GroomStats* st = groom_stats();
    if (f == 90) T.have_a = head_palette(w, T.pal_a, &T.nb_a);
    if (f == 100) editor_select(ed, T.e, SEL_REPLACE); // (the actor selected: its outline, its hair's inside it)
    (void)vk;
    if (f > 110 && f < 150) T.mask_on += st->mask_draws; // (its hair drawn into the selection's mask: selected and on)
    if (f > 160 && f < 176) T.mask_off += st->mask_draws; // (off: none)
    if (f >= 90 && f < 150 && st->skinned) T.skinned_seen = true;
    if (f == 135) T.have_b = head_palette(w, T.pal_b, &T.nb_b);
    if (f == 150) g->visible = 0;
    // (the toggling class's own Groom counted apart: the actor's off leaves only that one)
    const CGroom* og = T.bp && ecs_alive(w, T.bp) ? ecs_get(w, T.bp, COMP_GROOM) : NULL;
    uint32_t others = og && og->visible ? 1u : 0u;
    if (f == 175) T.off_empty = st->grooms == others;
    if (f == 176) g->visible = 1;
    if (f == 200) T.on_again = st->grooms > others;
}

bool groom_skin_test_check(Editor* ed) {
    (void)ed;
    bool ok = true;
    float moved = 0.0f;
    for (uint32_t b = 0; b < T.nb_a && b < T.nb_b; b++)
        for (int r = 0; r < 3; r++)
            for (int c = 0; c < 4; c++) moved = fmaxf(moved, fabsf(T.pal_a[b][r][c] - T.pal_b[b][r][c]));
    printf("[RealEngine] Groom on a skin: on the Skeletal Mesh's actor %s, bent by its bones %s, its bone moved %.3f, off: %s, on again: %s; "
           "its hair in the selection's mask %u frames on, %u off\n",
           T.on_sk ? "yes" : "no", T.skinned_seen ? "yes" : "no", moved, T.off_empty ? "no hair" : "STILL DRAWN", T.on_again ? "drawn" : "NOT DRAWN",
           T.mask_on, T.mask_off);
    if (!T.mask_on || T.mask_off) printf("[RealEngine] FAIL: groom skin: the hair isn't in the selection's outline (or still there when off)\n"), ok = false;
    if (!T.e) printf("[RealEngine] FAIL: groom skin: no Groom imported\n"), ok = false;
    if (!T.on_sk) printf("[RealEngine] FAIL: groom skin: the Groom isn't on the Skeletal Mesh's actor\n"), ok = false;
    if (!T.skinned_seen) printf("[RealEngine] FAIL: groom skin: its strands weren't bent by the skin's bones\n"), ok = false;
    if (!T.have_a || !T.have_b) printf("[RealEngine] FAIL: groom skin: its Skeletal Mesh has no pose\n"), ok = false;
    else if (moved < 1e-3f) printf("[RealEngine] aviso: groom skin: the animation moved none of its bones between the two moments (nothing to follow; skipped)\n");
    if (!T.off_empty) printf("[RealEngine] FAIL: groom skin: Visible off still drew the hair\n"), ok = false;
    if (!T.on_again) printf("[RealEngine] FAIL: groom skin: Visible on again drew nothing\n"), ok = false;
    printf("[RealEngine] Groom nodes (a Real class): Simulate on %s (simulated %s), Visible off %s, back on and static %s\n", T.bp_sim_on ? "yes" : "no",
           T.bp_simulated ? "yes" : "no", T.bp_off ? "yes" : "no", T.bp_back ? "yes" : "no");
    if (!T.bp_sim_on || !T.bp_simulated || !T.bp_off || !T.bp_back) printf("[RealEngine] FAIL: groom skin: the Groom nodes didn't turn the hair's simulation and visibility\n"), ok = false;
    if (T.frame < 200) printf("[RealEngine] FAIL: groom skin: the measure too short (%u frames)\n", T.frame), ok = false;
    if (ok) printf("[RealEngine] Groom skin OK\n");
    return ok;
}
