// (Q16) The test of everything editable in the game (realengine --ingame-test loading|progressive --ingame-package DIR,
// ctest ingame_package_* and game_bake_*; notas-dev/em-jogo.md):
//   * props.c and cvars.c checked in C (names, qualified names, vectors, flags, ranges, a play's settings given back) and
//     timed (a Set / Get Property by Name, a Set / Get Console Variable, in ns);
//   * a level whose map is made while playing (the procedural Facility: rooms, walls, doors, a light a room, all Static)
//     with a Blueprint that at BeginPlay sets a console variable and a property of its own light by name, reads them back,
//     then asks Build Lighting (Async) in the mode given: its Completed and Event On Lighting Built print; packaged, the
//     game plays it (the bake in the game, behind its loading screen or while it plays).
#include "ingame_test.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "bp_internal.h"
#include "components.h"
#include "content.h"
#include "cvars.h"
#include "editor.h"
#include "platform.h"
#include "platform_files.h"
#include "procgen.h"
#include "project.h"
#include "props.h"

static bool g_fail;

static void check(bool ok, const char* what) {
    if (!ok) printf("[RealEngine] FAIL: in-game test: %s\n", what), g_fail = true;
}

// --- props.c and cvars.c, in C ---
static void self_test(Editor* ed) {
    World* w = &ed->world;
    check(prop_count() > 300, "fewer than 300 Details properties reach the game");
    check(cvar_count() > 60, "fewer than 60 console variables");
    const PropInfo* p = prop_find(-1, "Intensity");
    check(p && p->comp == COMP_LIGHT, "\"Intensity\" isn't the light's");
    check(prop_find(COMP_FOG, "Fog Density") != NULL, "the fog's density");
    check(prop_find(-1, "max force") != NULL, "a label with its parenthesis cut (\"Max Force (0: any)\")");
    check(prop_find(-1, "Character Movement.Max Walk Speed") != NULL, "a name with its component");
    check(prop_find(-1, "No Such Property") == NULL, "an unknown name found");
    // on an actor: a light (a number, a vector, a flag), and a Blueprint-like actor whose light is a component of it
    Entity e = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_LIGHT));
    float v[4];
    check(prop_set(w, e, "Intensity", 42.0f, NULL) && ((CLight*)ecs_get(w, e, COMP_LIGHT))->intensity == 42.0f, "Set Property Intensity");
    check(prop_get(w, e, "light.intensity", v) && v[0] == 42.0f, "Get Property light.intensity");
    check(prop_set(w, e, "Light Color", 0.0f, (float[3]){ 0.25f, 0.5f, 1.0f }) && ((CLight*)ecs_get(w, e, COMP_LIGHT))->color[2] == 1.0f, "a color by Vector");
    check(prop_set(w, e, "Cast Shadows", 0.0f, NULL) && ((CLight*)ecs_get(w, e, COMP_LIGHT))->cast_shadows == 0, "a checkbox (uint32 flag)");
    check(!prop_set(w, e, "Max Walk Speed", 1.0f, NULL), "a property of a component the actor hasn't");
    // a checkbox that is a bit (inverted: Collision Enabled clears TRANSFORM_NO_COLLISION), the Mobility's value
    CTransform* et = ecs_get(w, e, COMP_TRANSFORM);
    et->flags = MOBILITY_STATIC | TRANSFORM_NO_ICON;
    check(prop_set(w, e, "Collision Enabled", 0.0f, NULL) && (et->flags & TRANSFORM_NO_COLLISION) && (et->flags & TRANSFORM_NO_ICON),
          "Collision Enabled off: its bit set, the others kept");
    check(prop_set(w, e, "Mobility", 2.0f, NULL) && (et->flags & TRANSFORM_MOBILITY_MASK) == MOBILITY_MOVABLE && (et->flags & TRANSFORM_NO_COLLISION),
          "Mobility 2: Movable");
    check(prop_get(w, e, "Show Icon", v) && v[0] == 0.0f, "Show Icon read as off");
    Entity part = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_FOG) | COMP_BIT(COMP_ATTACH));
    ((CAttach*)ecs_get(w, part, COMP_ATTACH))->owner = e;
    check(prop_set(w, e, "Fog Density", 0.3f, NULL) && ((CFog*)ecs_get(w, part, COMP_FOG))->density == 0.3f, "a property of an attached component");
    // the cost of a call (a node's whole work but the VM's)
    const int N = 100000;
    double t0 = re_time_seconds();
    for (int i = 0; i < N; i++) prop_set(w, e, "Attenuation Radius", (float)(i & 1023), NULL);
    double t1 = re_time_seconds();
    for (int i = 0; i < N; i++) prop_get(w, e, "Attenuation Radius", v);
    double t2 = re_time_seconds();
    for (int i = 0; i < N; i++) prop_set(w, e, "Fog Density", 0.01f, NULL); // (through the attached component: the archetypes walked)
    double t3 = re_time_seconds();
    ecs_destroy(w, part), ecs_destroy(w, e);
    // the console variables on a view of the test's own, a play's settings given back
    static GiSettings gi;
    int msaa = 4;
    memset(&gi, 0, sizeof gi);
    gi.probes = true, gi.intensity = 1.0f;
    cvar_bind_view(&gi, &msaa);
    float g0 = project()->gravity_z;
    cvar_begin_play();
    check(cvar_set("r.GI.Probes", 0.0f, NULL) && !gi.probes, "r.GI.Probes 0");
    check(cvar_set("R.gi.intensity", 0.0f, "1.5") && gi.intensity == 1.5f, "r.GI.Intensity by text, any case");
    char out[128];
    check(cvar_exec("r.MSAA 3", out, sizeof out) && msaa == 2, "r.MSAA 3 -> 2");
    check(cvar_exec("p.GravityZ 490", out, sizeof out) && project()->gravity_z == 490.0f && !strcmp(out, "p.GravityZ = 490"), "p.GravityZ 490");
    float x = 0.0f;
    check(cvar_get("r.Shadow.Virtual.SunAngle", &x, NULL, 0), "a GI field read");
    check(!cvar_exec("r.NoSuchThing 1", out, sizeof out), "an unknown command accepted");
    double t4 = re_time_seconds();
    for (int i = 0; i < N; i++) cvar_set("r.GI.Intensity", 1.0f + (float)(i & 7) * 0.1f, NULL);
    double t5 = re_time_seconds();
    for (int i = 0; i < N; i++) cvar_get("r.GI.Intensity", &x, NULL, 0);
    double t6 = re_time_seconds();
    cvar_end_play();
    check(gi.probes && gi.intensity == 1.0f && msaa == 4 && project()->gravity_z == g0, "the play's settings given back at its end");
    cvar_bind_view(&ed->gi, &ed->msaa);
    printf("[RealEngine] In-game settings: %d Details properties, %d console variables; Set Property by Name %.0f ns, Get %.0f ns, "
           "on a component %.0f ns; Set Console Variable %.0f ns, Get %.0f ns\n",
           prop_count(), cvar_count(), (t1 - t0) * 1e9 / N, (t2 - t1) * 1e9 / N, (t3 - t2) * 1e9 / N, (t5 - t4) * 1e9 / N, (t6 - t5) * 1e9 / N);
}

// --- the level and its Blueprint ---
static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] FAIL: in-game test: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_fail = true;
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin(g, a, 1, ap), b, (uint8_t)pin(g, b, 0, bpn))) {
        printf("[RealEngine] FAIL: in-game test: link %u.%s -> %u.%s refused\n", a, ap, b, bpn);
        g_fail = true;
    }
}
static uint32_t node(BpAsset* a, BpGraph* g, uint16_t kind, float x) {
    BpNode* n = bp_add_node(a, g, kind, fmodf(x, 6.0f) * 300.0f, floorf(x / 6.0f) * 190.0f, NULL, NULL);
    if (!n) g_fail = true;
    return n ? n->id : 0;
}
static void lit(BpGraph* g, uint32_t id, const char* name, float x) {
    BpNode* n = bp_node(g, id);
    int p = pin(g, id, 0, name);
    if (n) n->pins[p].lit = (BpValue){ { x, 0, 0, 0 } };
}
static void slit(BpGraph* g, uint32_t id, const char* name, const char* s) {
    BpNode* n = bp_node(g, id);
    int p = pin(g, id, 0, name);
    if (n) snprintf(n->pins[p].slit, sizeof n->pins[p].slit, "%s", s);
}
static uint32_t print(BpAsset* a, BpGraph* g, float x, const char* s) {
    uint32_t p = node(a, g, BPK_PRINT, x);
    slit(g, p, "In String", s);
    return p;
}

// BeginPlay -> Set Console Variable r.GI.Intensity 1.25 -> Branch(Get Console Variable == 1.25) -> print / FAIL
//   -> Set Property by Name (self) Intensity 42 -> Branch(Get Property by Name Light.Intensity == 42) -> print / FAIL
//   -> Build Lighting (Async)(mode, Preview): Completed -> print. Event On Lighting Built -> Branch(Success) -> print / FAIL
static BpAsset* make_class(const char* F, const char* mode) {
    BpAsset* a = bp_create(BPA_CLASS, F, "BP_InGameTest", "Actor");
    if (!a) return NULL;
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t begin = node(a, g, BPK_EVENT_BEGIN_PLAY, 0);
    uint32_t setc = node(a, g, BPK_SET_CVAR, 1);
    slit(g, setc, "Name", "r.GI.Intensity"), lit(g, setc, "Value", 1.25f);
    uint32_t getc = node(a, g, BPK_GET_CVAR, 7);
    slit(g, getc, "Name", "r.gi.intensity");
    uint32_t eqc = node(a, g, BPK_EQUAL, 8);
    lit(g, eqc, "B", 1.25f);
    uint32_t brc = node(a, g, BPK_BRANCH, 2);
    uint32_t okc = print(a, g, 3, "In-game console variable: OK");
    uint32_t badc = print(a, g, 9, "FAIL: in-game console variable");
    link(g, begin, "", setc, "");
    link(g, setc, "", brc, "");
    link(g, getc, "Value", eqc, "A");
    link(g, eqc, "", brc, "Condition");
    link(g, brc, "True", okc, "");
    link(g, brc, "False", badc, "");
    uint32_t setp = node(a, g, BPK_SET_PROPERTY, 4);
    slit(g, setp, "Property", "Intensity"), lit(g, setp, "Value", 42.0f);
    uint32_t getp = node(a, g, BPK_GET_PROPERTY, 10);
    slit(g, getp, "Property", "Light.Intensity");
    uint32_t eqp = node(a, g, BPK_EQUAL, 11);
    lit(g, eqp, "B", 42.0f);
    uint32_t brp = node(a, g, BPK_BRANCH, 5);
    uint32_t okp = print(a, g, 12, "In-game property: OK");
    uint32_t badp = print(a, g, 13, "FAIL: in-game property");
    link(g, okc, "", setp, "");
    link(g, setp, "", brp, "");
    link(g, getp, "Value", eqp, "A");
    link(g, eqp, "", brp, "Condition");
    link(g, brp, "True", okp, "");
    link(g, brp, "False", badp, "");
    uint32_t bl = node(a, g, BPK_BUILD_LIGHTING, 14);
    slit(g, bl, "Mode", mode), slit(g, bl, "Quality", "Preview");
    uint32_t started = print(a, g, 15, "Runtime lighting: started");
    uint32_t done = print(a, g, 16, "Runtime lighting: completed");
    if (!strcmp(mode, "Progressive")) { // (while playing: once the game runs warm, its first frames' own costs apart from the bake's)
        uint32_t dl = node(a, g, BPK_DELAY, 17);
        lit(g, dl, "Duration", 1.5f);
        link(g, okp, "", dl, "");
        link(g, dl, "Completed", bl, "");
    } else
        link(g, okp, "", bl, "");
    link(g, bl, "", started, "");
    link(g, bl, "Completed", done, "");
    uint32_t ev = node(a, g, BPK_EVENT_LIGHTING_BUILT, 18);
    uint32_t bre = node(a, g, BPK_BRANCH, 19);
    uint32_t oke = print(a, g, 20, "Runtime lighting built (Event On Lighting Built)");
    uint32_t bade = print(a, g, 21, "FAIL: runtime lighting not built");
    link(g, ev, "", bre, "");
    link(g, ev, "Success", bre, "Condition");
    link(g, bre, "True", oke, "");
    link(g, bre, "False", bade, "");
    if (g_fail || !bp_save(a) || !bp_compile(a)) {
        printf("[RealEngine] FAIL: in-game test: BP_InGameTest didn't compile: %s\n", a->error);
        return NULL;
    }
    return a;
}

bool ingame_test_setup(Editor* ed, const char* mode, const char* package_dir) {
    g_fail = false;
    self_test(ed);
    bool loading = mode && (!strcmp(mode, "loading") || !strcmp(mode, "Loading Screen"));
    const char* F = "/Content/InGameTest";
    char dir[1100];
    snprintf(dir, sizeof dir, "%s/InGameTest", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    BpAsset* cls = make_class(F, loading ? "Loading Screen" : "Progressive");
    if (!cls) return false;
    World* w = &ed->world;
    // the map made while playing: the Facility (its rooms, walls with doors, lights: Static), a light the Blueprint owns
    Entity pw = ecs_create(w, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_PROC_WORLD));
    if (!pw) return false;
    snprintf(((CName*)ecs_get(w, pw, COMP_NAME))->text, RE_NAME_MAX, "Facility");
    CTransform* t = ecs_get(w, pw, COMP_TRANSFORM);
    t->location[0] = 0, t->location[1] = 1500, t->location[2] = 0, t->scale[0] = t->scale[1] = t->scale[2] = 1;
    CProcWorld* c = ecs_get(w, pw, COMP_PROC_WORLD);
    c->mode = PROCGEN_FACILITY, c->seed = 7, c->rooms = 10, c->room_size = 800.0f, c->room_height = 350.0f, c->loop_chance = 0.2f;
    c->creature_density = 0.0f;
    Entity act = bp_spawn(w, cls, (float[3]){ 0, 1500, 200 }, (float[3]){ 0, 0, 0 });
    if (!act || !ecs_add(w, act, COMP_LIGHT)) return false;
    CLight* l = ecs_get(w, act, COMP_LIGHT);
    l->type = LIGHT_POINT, l->intensity = 5.0f, l->attenuation_radius = 600.0f, l->color[0] = l->color[1] = l->color[2] = 1.0f;
    CTransform* at = ecs_get(w, act, COMP_TRANSFORM);
    if (at) at->flags = (at->flags & ~TRANSFORM_MOBILITY_MASK) | MOBILITY_MOVABLE;
    if (package_dir) {
        char lvl[1200];
        re_make_dirs(package_dir);
        snprintf(lvl, sizeof lvl, "%s/%s.rlevel", package_dir, loading ? "BakeLoading" : "BakeProgressive");
        if (!editor_save_level(ed, lvl, 0.0) || !editor_package_game(ed, package_dir, false, 0.0)) {
            printf("[RealEngine] FAIL: in-game test: packaging into %s failed: %s\n", package_dir, ed->status_msg);
            return false;
        }
    }
    printf("[RealEngine] In-game settings: %s\n", g_fail ? "FAIL" : "PASS");
    return !g_fail;
}
