// The Blueprints' test (realengine --bp-test, ctest "blueprints"): assets made through the API the editor uses, played
// in the editor, the world checked afterwards. What it covers: events (BeginPlay, Tick, interface), For Loop, Branch,
// Delay, Switch on Enum, a function with a parameter and a result, variables (integer, float, structure), Make / Break
// Struct, a Data Asset, an interface message, and the Character template (walks, falls onto the floor).
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp_editor.h"
#include "bp_internal.h"
#include "bp_timeline.h"
#include "gameplay_tags.h"
#include "components.h"
#include "content.h"
#include "editor.h"
#include "platform_files.h"
#include "level.h"
#include "input.h"
#include "post.h"

static bool g_fail;
static Entity g_spawner; // (Expose on Spawn: the actor that spawns BP_TestSpawned with Size 3)
static Entity g_timeline; // (2026-11-16) BP_TestTimeline: a Timeline played from BeginPlay
static Entity g_actor, g_char, g_types, g_input, g_ppv, g_watch_begin, g_watch_end, g_trigger, g_disp_listener, g_disp_source;

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    printf("[RealEngine] bp-test: node %u has no %s pin \"%s\"\n", id, out ? "output" : "input", name);
    g_fail = true;
    return 0;
}

static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    int pa = pin(g, a, 1, ap), pb = pin(g, b, 0, bpn);
    if (!bp_link(g, a, (uint8_t)pa, b, (uint8_t)pb)) {
        printf("[RealEngine] bp-test: link %u.%s -> %u.%s refused\n", a, ap, b, bpn);
        g_fail = true;
    }
}

static uint32_t node(BpAsset* a, BpGraph* g, uint16_t kind, float x, const char* text, const char* text2) {
    BpNode* n = bp_add_node(a, g, kind, fmodf(x, 6.0f) * 300.0f, floorf(x / 6.0f) * 190.0f, text, text2); // x: the slot in a grid
    if (!n) g_fail = true;
    return n ? n->id : 0;
}

static void lit(BpGraph* g, uint32_t id, const char* name, float x, float y, float z) {
    BpNode* n = bp_node(g, id);
    int p = pin(g, id, 0, name);
    if (n) n->pins[p].lit = (BpValue){ { x, y, z, 0 } };
}

static void slit(BpGraph* g, uint32_t id, const char* name, const char* s) {
    BpNode* n = bp_node(g, id);
    int p = pin(g, id, 0, name);
    if (n) snprintf(n->pins[p].slit, sizeof n->pins[p].slit, "%s", s);
}

// The newer types: an array (Make Array, Add, For Each Loop), strings made while playing (Append, Equal), a transform
// pin split into its parts, a macro (exec and data through it), an interface function with an input and an output.
// BeginPlay: Arr = [1, 2, 3] + 4; Sum of them = 10; Set Actor Transform(Location (Sum, Length 4, Twice(Select(Append
// ("Re","al") == "Real", 7, 0)) = 14), Rotation yaw 45); Hit(21) (Message) -> Doubled 42 -> Set Actor Scale 3D (42, 1, 1)
static BpAsset* make_types_class(const char* F) {
    BpAsset* bi = bp_create(BPA_INTERFACE, F, "BPI_Hit", NULL);
    BpAsset* a = bp_create(BPA_CLASS, F, "BP_TestTypes", "Actor");
    if (!bi || !a) return NULL;
    BpGraph* ig = &bi->graphs[0]; // the interface's function: Hit(Amount) -> Doubled
    snprintf(ig->name, BP_NAME, "Hit");
    snprintf(bi->entries[0], BP_NAME, "Hit");
    ig->nparams = 1, ig->nresults = 1;
    memset(&ig->params[0], 0, sizeof ig->params[0]), memset(&ig->results[0], 0, sizeof ig->results[0]);
    snprintf(ig->params[0].name, BP_NAME, "Amount"), ig->params[0].type = BPT_FLOAT;
    snprintf(ig->results[0].name, BP_NAME, "Doubled"), ig->results[0].type = BPT_FLOAT;
    if (!bp_save(bi)) return NULL;
    snprintf(a->interfaces[a->ninterfaces++], BP_NAME, "BPI_Hit");
    BpVar* v = &a->vars[a->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "Arr"), v->type = BPT_FLOAT, v->cont = BPC_ARRAY;
    v = &a->vars[a->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "Sum"), v->type = BPT_FLOAT;
    // the interface function implemented (it has an output: a function graph, Unreal's way)
    BpGraph* hg = bp_add_graph(a, "Hit", BPG_FUNCTION);
    hg->nparams = 1, hg->nresults = 1;
    hg->params[0] = ig->params[0], hg->results[0] = ig->results[0];
    snprintf(hg->iface, BP_NAME, "BPI_Hit");
    for (uint32_t i = 0; i < hg->nnodes; i++) bp_refresh_pins(a, hg, &hg->nodes[i]);
    uint32_t hin = hg->nodes[0].id, hret = hg->nodes[1].id, hmul = node(a, hg, BPK_MUL, 1, NULL, NULL);
    link(hg, hin, "", hret, "");
    link(hg, hin, "Amount", hmul, "A");
    lit(hg, hmul, "B", 2, 0, 0);
    link(hg, hmul, "", hret, "Doubled");
    // the macro Twice: In -> Out, V -> W = V * 2
    BpGraph* mg = bp_add_graph(a, "Twice", BPG_MACRO);
    mg->nparams = 2, mg->nresults = 2;
    memset(&mg->params[1], 0, sizeof mg->params[1]), memset(&mg->results[1], 0, sizeof mg->results[1]);
    snprintf(mg->params[1].name, BP_NAME, "V"), mg->params[1].type = BPT_FLOAT;
    snprintf(mg->results[1].name, BP_NAME, "W"), mg->results[1].type = BPT_FLOAT;
    for (uint32_t i = 0; i < mg->nnodes; i++) bp_refresh_pins(a, mg, &mg->nodes[i]);
    uint32_t tin = mg->nodes[0].id, tout = mg->nodes[1].id, mmul = node(a, mg, BPK_MUL, 1, NULL, NULL);
    link(mg, tin, "In", tout, "Out");
    link(mg, tin, "V", mmul, "A");
    lit(mg, mmul, "B", 2, 0, 0);
    link(mg, mmul, "", tout, "W");
    // the Event Graph
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t begin = node(a, g, BPK_EVENT_BEGIN_PLAY, 0, NULL, NULL);
    uint32_t mk = node(a, g, BPK_ARR_MAKE, 1, NULL, NULL);
    bp_node(g, mk)->extra = 1, bp_refresh_pins(a, g, bp_node(g, mk));
    uint32_t f1 = node(a, g, BPK_VAR_GET, 2, "Sum", NULL); // a float to type the wildcard
    link(g, f1, "Sum", mk, "[0]");
    lit(g, mk, "[1]", 2, 0, 0), lit(g, mk, "[2]", 3, 0, 0);
    uint32_t set_arr = node(a, g, BPK_VAR_SET, 3, "Arr", NULL);
    link(g, begin, "", set_arr, "");
    link(g, mk, "Array", set_arr, "Arr");
    uint32_t one = node(a, g, BPK_VAR_SET, 4, "Sum", NULL); // (Sum is 0 while the array is made: [0] is 0, set to 1 after)
    link(g, set_arr, "", one, "");
    uint32_t get_arr = node(a, g, BPK_VAR_GET, 5, "Arr", NULL);
    uint32_t setel = node(a, g, BPK_ARR_SET, 6, NULL, NULL);
    link(g, one, "", setel, "");
    link(g, get_arr, "Arr", setel, "Target Array");
    lit(g, setel, "Item", 1, 0, 0);
    uint32_t add = node(a, g, BPK_ARR_ADD, 7, NULL, NULL);
    link(g, setel, "", add, "");
    link(g, get_arr, "Arr", add, "Target Array");
    lit(g, add, "New Item", 4, 0, 0);
    uint32_t each = node(a, g, BPK_FOREACH, 8, NULL, NULL);
    link(g, add, "", each, "");
    link(g, get_arr, "Arr", each, "Array");
    uint32_t gsum = node(a, g, BPK_VAR_GET, 9, "Sum", NULL), plus = node(a, g, BPK_ADD, 10, NULL, NULL);
    uint32_t ssum = node(a, g, BPK_VAR_SET, 11, "Sum", NULL);
    link(g, each, "Loop Body", ssum, "");
    link(g, gsum, "Sum", plus, "A");
    link(g, each, "Array Element", plus, "B");
    link(g, plus, "", ssum, "Sum");
    // Completed -> Twice -> Set Actor Transform (its New Transform split)
    uint32_t tw = node(a, g, BPK_MACRO, 12, "Twice", NULL);
    link(g, each, "Completed", tw, "In");
    uint32_t app = node(a, g, BPK_APPEND, 13, NULL, NULL);
    slit(g, app, "A", "Re"), slit(g, app, "B", "al");
    uint32_t eq = node(a, g, BPK_STR_EQUAL, 14, NULL, NULL);
    link(g, app, "Return Value", eq, "A");
    slit(g, eq, "B", "Real");
    uint32_t sel = node(a, g, BPK_SELECT, 15, NULL, NULL);
    link(g, eq, "", sel, "Pick A");
    lit(g, sel, "A", 7, 0, 0);
    link(g, sel, "", tw, "V");
    uint32_t len = node(a, g, BPK_ARR_LEN, 16, NULL, NULL);
    link(g, get_arr, "Arr", len, "Target Array");
    uint32_t gsum2 = node(a, g, BPK_VAR_GET, 17, "Sum", NULL);
    uint32_t vec = node(a, g, BPK_MAKE_VECTOR, 18, NULL, NULL);
    link(g, gsum2, "Sum", vec, "X");
    link(g, len, "", vec, "Y");
    link(g, tw, "W", vec, "Z");
    uint32_t sxf = node(a, g, BPK_SET_TRANSFORM, 19, NULL, NULL);
    BpNode* sn = bp_node(g, sxf);
    if (!sn || !bp_split_pin(a, g, sn, (uint8_t)pin(g, sxf, 0, "New Transform"), true)) g_fail = true;
    link(g, tw, "Out", sxf, "");
    link(g, vec, "", sxf, "New Transform Location");
    lit(g, sxf, "New Transform Rotation", 0, 45, 0);
    // Hit(21) (Message) on self -> Set Actor Scale 3D (Doubled, 1, 1)
    uint32_t hit = node(a, g, BPK_CALL_INTERFACE, 20, "BPI_Hit", "Hit");
    link(g, sxf, "", hit, "");
    lit(g, hit, "Amount", 21, 0, 0);
    uint32_t sv = node(a, g, BPK_MAKE_VECTOR, 21, NULL, NULL);
    link(g, hit, "Doubled", sv, "X");
    lit(g, sv, "Y", 1, 0, 0), lit(g, sv, "Z", 1, 0, 0);
    uint32_t ss = node(a, g, BPK_SET_SCALE, 22, NULL, NULL);
    link(g, hit, "", ss, "");
    link(g, sv, "", ss, "New Scale 3D");
    if (g_fail || !bp_save(a)) return NULL;
    if (!bp_compile(a)) {
        printf("[RealEngine] FAIL: bp-test: BP_TestTypes didn't compile: %s\n", a->error);
        return NULL;
    }
    printf("[RealEngine] bp-test: BP_TestTypes %u ops, %u registers, %u containers\n", a->ncode, a->nslots, a->narr);
    return a;
}

// Enhanced Input: BeginPlay adds IMC_Default; IA_Move's Triggered sets the scale's z to 1 + 2 * its Y (W held: 3)
static BpAsset* make_input_class(const char* F) {
    BpAsset* a = bp_create(BPA_CLASS, F, "BP_TestInput", "Actor");
    if (!a) return NULL;
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t begin = node(a, g, BPK_EVENT_BEGIN_PLAY, 0, NULL, NULL);
    uint32_t add = node(a, g, BPK_ADD_MAPPING_CONTEXT, 1, "IMC_Default", NULL);
    link(g, begin, "", add, "");
    uint32_t ev = node(a, g, BPK_EVENT_INPUT_ACTION, 2, "IA_Move", NULL);
    uint32_t brk = node(a, g, BPK_BREAK_VECTOR, 3, NULL, NULL);
    link(g, ev, "Action Value", brk, "");
    uint32_t mul = node(a, g, BPK_MUL, 4, NULL, NULL), plus = node(a, g, BPK_ADD, 5, NULL, NULL);
    link(g, brk, "Y", mul, "A");
    lit(g, mul, "B", 2, 0, 0);
    link(g, mul, "", plus, "A");
    lit(g, plus, "B", 1, 0, 0);
    uint32_t mk = node(a, g, BPK_MAKE_VECTOR, 6, NULL, NULL);
    lit(g, mk, "X", 1, 0, 0), lit(g, mk, "Y", 1, 0, 0);
    link(g, plus, "", mk, "Z");
    uint32_t ss = node(a, g, BPK_SET_SCALE, 7, NULL, NULL);
    link(g, ev, "Triggered", ss, "");
    link(g, mk, "", ss, "New Scale 3D");
    if (g_fail || !bp_save(a) || !bp_compile(a)) {
        printf("[RealEngine] FAIL: bp-test: BP_TestInput didn't compile: %s\n", a->error);
        return NULL;
    }
    return a;
}

// Overlaps: a watcher whose Event ActorBeginOverlap (or End) of the actor named "TestTrigger" (the Level Blueprint's way) sets
// its scale to s: the Character walks through that Trigger Box
static BpAsset* make_watch_class(const char* F, const char* name, uint16_t kind, float s) {
    BpAsset* a = bp_create(BPA_CLASS, F, name, "Actor");
    if (!a) return NULL;
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t ev = node(a, g, kind, 0, "TestTrigger", NULL);
    uint32_t mk = node(a, g, BPK_MAKE_VECTOR, 1, NULL, NULL);
    lit(g, mk, "X", s, 0, 0), lit(g, mk, "Y", s, 0, 0), lit(g, mk, "Z", s, 0, 0);
    uint32_t ss = node(a, g, BPK_SET_SCALE, 2, NULL, NULL);
    link(g, ev, "", ss, "");
    link(g, mk, "", ss, "New Scale 3D");
    if (g_fail || !bp_save(a) || !bp_compile(a)) {
        printf("[RealEngine] FAIL: bp-test: %s didn't compile: %s\n", name, a->error);
        return NULL;
    }
    return a;
}

// Event Dispatchers: the source has OnPing (Amount: float) and calls it half a second after BeginPlay; the listener
// finds the source (Get Actor Of Class), binds its Event OnPing to it, and sets its own scale to Amount (4) when called
static BpAsset* make_dispatch_source(const char* F) {
    BpAsset* a = bp_create(BPA_CLASS, F, "BP_TestDispSource", "Actor");
    if (!a) return NULL;
    BpGraph* d = bp_add_graph(a, "OnPing", BPG_DISPATCHER);
    if (!d) return NULL;
    d->nparams = 1;
    memset(&d->params[0], 0, sizeof d->params[0]);
    snprintf(d->params[0].name, BP_NAME, "Amount");
    d->params[0].type = BPT_FLOAT;
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t bp = node(a, g, BPK_EVENT_BEGIN_PLAY, 0, NULL, NULL);
    uint32_t dl = node(a, g, BPK_DELAY, 1, NULL, NULL);
    lit(g, dl, "Duration", 0.5f, 0, 0);
    uint32_t call = node(a, g, BPK_DISPATCH_CALL, 2, "OnPing", NULL);
    lit(g, call, "Amount", 4.0f, 0, 0);
    link(g, bp, "", dl, "");
    link(g, dl, "Completed", call, "");
    for (uint32_t k = 1; k < a->ngraphs; k++) // its Construction Script turns it (yaw 33) before BeginPlay
        if (!strcmp(a->graphs[k].name, "ConstructionScript")) {
            BpGraph* cs = &a->graphs[k];
            uint32_t entry = 0;
            for (uint32_t i = 0; i < cs->nnodes; i++)
                if (cs->nodes[i].kind == BPK_FUNC_ENTRY) entry = cs->nodes[i].id;
            uint32_t sr = node(a, cs, BPK_SET_ROTATION, 1, NULL, NULL);
            lit(cs, sr, "New Rotation", 0, 33, 0);
            link(cs, entry, "", sr, "");
        }
    if (g_fail || !bp_save(a) || !bp_compile(a)) {
        printf("[RealEngine] FAIL: bp-test: BP_TestDispSource didn't compile: %s\n", a->error);
        return NULL;
    }
    return a;
}

static BpAsset* make_dispatch_listener(const char* F) {
    BpAsset* a = bp_create(BPA_CLASS, F, "BP_TestDispListener", "Actor");
    if (!a) return NULL;
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t bp = node(a, g, BPK_EVENT_BEGIN_PLAY, 0, NULL, NULL);
    uint32_t find = node(a, g, BPK_GET_ACTOR_OF_CLASS, 1, NULL, NULL);
    slit(g, find, "Actor Class", "BP_TestDispSource");
    uint32_t bind = node(a, g, BPK_DISPATCH_BIND, 2, "OnPing", "BP_TestDispSource");
    link(g, bp, "", bind, "");
    link(g, find, "Return Value", bind, "Target");
    uint32_t ev = node(a, g, BPK_EVENT_DISPATCH, 3, "OnPing", "BP_TestDispSource");
    uint32_t mk = node(a, g, BPK_MAKE_VECTOR, 4, NULL, NULL);
    link(g, ev, "Amount", mk, "X");
    link(g, ev, "Amount", mk, "Y");
    link(g, ev, "Amount", mk, "Z");
    uint32_t ss = node(a, g, BPK_SET_SCALE, 5, NULL, NULL);
    link(g, ev, "", ss, "");
    link(g, mk, "", ss, "New Scale 3D");
    // (2026-11-10) Unreal's way too: a Custom Event's red square (Delegate) wired into a second Bind Event to's Event;
    // the call runs it: the listener turns to yaw 77
    uint32_t bind2 = node(a, g, BPK_DISPATCH_BIND, 6, "OnPing", "BP_TestDispSource");
    link(g, bind, "", bind2, "");
    link(g, find, "Return Value", bind2, "Target");
    uint32_t cev = node(a, g, BPK_EVENT_CUSTOM, 7, "HandlePing", NULL);
    link(g, cev, "Delegate", bind2, "Event");
    uint32_t sr = node(a, g, BPK_SET_ROTATION, 8, NULL, NULL);
    lit(g, sr, "New Rotation", 0, 77, 0);
    link(g, cev, "", sr, "");
    if (g_fail || !bp_save(a) || !bp_compile(a)) {
        printf("[RealEngine] FAIL: bp-test: BP_TestDispListener didn't compile: %s\n", a->error);
        return NULL;
    }
    return a;
}

// (2026-11-10) Expose on Spawn: BP_TestSpawned's Size (exposed, default 1) scales it at BeginPlay; BP_TestSpawner's BeginPlay
// spawns one with Size 3 on the pin: 3 when the value arrives before BeginPlay (Unreal's order)
static BpAsset* make_spawn_classes(const char* F) {
    BpAsset* t = bp_create(BPA_CLASS, F, "BP_TestSpawned", "Actor");
    if (!t) return NULL;
    BpVar* v = &t->vars[t->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "Size"), v->type = BPT_FLOAT, v->expose_spawn = true, v->def.v[0] = 1.0f;
    BpGraph* g = &t->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t bpn = node(t, g, BPK_EVENT_BEGIN_PLAY, 0, NULL, NULL);
    uint32_t get = node(t, g, BPK_VAR_GET, 1, "Size", NULL);
    uint32_t mk = node(t, g, BPK_MAKE_VECTOR, 2, NULL, NULL);
    uint32_t ss = node(t, g, BPK_SET_SCALE, 3, NULL, NULL);
    link(g, get, "Size", mk, "X"), link(g, get, "Size", mk, "Y"), link(g, get, "Size", mk, "Z");
    link(g, bpn, "", ss, "");
    link(g, mk, "", ss, "New Scale 3D");
    if (g_fail || !bp_save(t) || !bp_compile(t)) {
        printf("[RealEngine] FAIL: bp-test: BP_TestSpawned didn't compile: %s\n", t->error);
        return NULL;
    }
    BpAsset* a = bp_create(BPA_CLASS, F, "BP_TestSpawner", "Actor");
    if (!a) return NULL;
    g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t b2 = node(a, g, BPK_EVENT_BEGIN_PLAY, 0, NULL, NULL);
    uint32_t sp = node(a, g, BPK_SPAWN, 1, NULL, NULL);
    slit(g, sp, "Class", "BP_TestSpawned");
    BpNode* spn = bp_node(g, sp);
    if (spn) bp_refresh_pins(a, g, spn); // (the class chosen: its exposed Size becomes a pin)
    lit(g, sp, "Size", 3.0f, 0, 0);
    link(g, b2, "", sp, "");
    if (g_fail || !bp_save(a) || !bp_compile(a)) {
        printf("[RealEngine] FAIL: bp-test: BP_TestSpawner didn't compile: %s\n", a->error);
        return NULL;
    }
    return a;
}

// (2026-11-16) A Timeline: BeginPlay -> Play from Start; its Float track Alpha 0 -> 2 over 1 s (linear) into Value on
// Update; its Event track Ping (a key at 0.5 s) sets Pings 1; Finished sets Done 1
static BpAsset* make_timeline_class(const char* F, bool idle) {
    // (its own name per mode: the blueprints and play_flicker tests run side by side and wrote the same file)
    BpAsset* a = bp_create(BPA_CLASS, F, idle ? "BP_TestTimelineIdle" : "BP_TestTimeline", "Actor");
    if (!a) return NULL;
    bp_timelines_free(a); // (a file left by an earlier run: its timelines start over)
    bp_add_var(a, "Value", BPT_FLOAT), bp_add_var(a, "Pings", BPT_FLOAT), bp_add_var(a, "Done", BPT_FLOAT);
    BpTimeline* tl = bp_timeline_add(a);
    if (!tl || tl->ntracks != 1) return NULL;
    tl->tracks[0].keys[0].interp = TLI_LINEAR;
    tl->tracks[0].keys[1].v[0] = 2.0f;
    BpTlTrack* ev = bp_timeline_add_track(tl, TLT_EVENT);
    if (!ev) return NULL;
    snprintf(ev->name, sizeof ev->name, "Ping");
    bp_timeline_add_key(ev, 0.5f, NULL);
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t bp = node(a, g, BPK_EVENT_BEGIN_PLAY, 0, NULL, NULL);
    uint32_t t = node(a, g, BPK_TIMELINE, 1, tl->name, NULL);
    uint32_t sv = node(a, g, BPK_VAR_SET, 2, "Value", NULL);
    uint32_t sp = node(a, g, BPK_VAR_SET, 3, "Pings", NULL);
    lit(g, sp, "Pings", 1, 0, 0);
    uint32_t sd = node(a, g, BPK_VAR_SET, 4, "Done", NULL);
    lit(g, sd, "Done", 1, 0, 0);
    link(g, bp, "", t, "Play from Start");
    link(g, t, "Update", sv, "");
    link(g, t, "Alpha", sv, "Value");
    link(g, t, "Ping", sp, "");
    link(g, t, "Finished", sd, "");
    if (g_fail || !bp_save(a) || !bp_compile(a)) {
        printf("[RealEngine] FAIL: bp-test: BP_TestTimeline didn't compile: %s\n", a->error);
        return NULL;
    }
    // saved and read back: the timeline in the file
    const BpTimeline* back = bp_timeline_find(a, tl->name);
    if (!back || back->ntracks != 2) printf("[RealEngine] FAIL: bp-test: the timeline lost\n"), g_fail = true;
    return a;
}

bool bp_test_setup(Editor* ed, const char* package_dir) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/BpTest", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    const char* F = "/Content/BpTest";
    BpAsset* st = bp_create(BPA_STRUCT, F, "S_Test", NULL); // Value (float), Position (vector)
    BpAsset* en = bp_create(BPA_ENUM, F, "E_Test", NULL);   // NewEnumerator0..2
    BpAsset* in = bp_create(BPA_INTERFACE, F, "BPI_Test", NULL); // Interact
    BpAsset* da = bp_create(BPA_DATA, F, "DA_Test", NULL);  // Damage 10
    BpAsset* ac = bp_create(BPA_CLASS, F, "BP_TestActor", "Actor");
    BpAsset* ch = bp_create(BPA_CLASS, F, "BP_TestCharacter", "Character");
    if (!st || !en || !in || !da || !ac || !ch) {
        printf("[RealEngine] bp-test: could not create the assets in %s\n", dir);
        return false;
    }
    snprintf(ac->interfaces[ac->ninterfaces++], BP_NAME, "BPI_Test");
    // variables
    BpVar* v = &ac->vars[ac->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "Count"), v->type = BPT_INT;
    v = &ac->vars[ac->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "Total"), v->type = BPT_FLOAT;
    v = &ac->vars[ac->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "Stats"), v->type = BPT_STRUCT, snprintf(v->ref, BP_NAME, "S_Test");
    // Instance Editable: the placed actor sets its own Speed and Label (bp_test_check reads them while playing)
    v = &ac->vars[ac->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "Speed"), v->type = BPT_FLOAT, v->editable = true, v->def.v[0] = 1.0f;
    v = &ac->vars[ac->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "Stunned"), v->type = BPT_BOOL;
    v = &ac->vars[ac->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "Formatted"), v->type = BPT_TEXT;
    v = &ac->vars[ac->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "LastHit"), v->type = BPT_FLOAT;
    // the actors with a tag (2026-11-09): Get All Actors with Gameplay Tag(State) / with Tag(State.Stunned): their counts
    v = &ac->vars[ac->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "TagMatching"), v->type = BPT_INT;
    v = &ac->vars[ac->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "TagExact"), v->type = BPT_INT;
    // a Custom Event with an input: Hit(Amount) adds Amount to LastHit
    BpGraph* hs = bp_add_graph(ac, "Hit", BPG_EVENT);
    if (hs) memset(&hs->params[0], 0, sizeof hs->params[0]), snprintf(hs->params[0].name, BP_NAME, "Amount"), hs->params[0].type = BPT_FLOAT, hs->nparams = 1;
    v = &ac->vars[ac->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "Label"), v->type = BPT_STRING, v->editable = true;
    snprintf(v->sdef, sizeof v->sdef, "classe");
    // the function AddTwice(X) -> Result = X * 2
    bp_add_function(ac, "AddTwice");
    BpGraph* fg = &ac->graphs[ac->ngraphs - 1]; // (after the Construction Script)
    fg->nparams = 1;
    memset(&fg->params[0], 0, sizeof fg->params[0]);
    snprintf(fg->params[0].name, BP_NAME, "X"), fg->params[0].type = BPT_FLOAT;
    fg->nresults = 1;
    memset(&fg->results[0], 0, sizeof fg->results[0]);
    snprintf(fg->results[0].name, BP_NAME, "Result"), fg->results[0].type = BPT_FLOAT;
    for (uint32_t i = 0; i < fg->nnodes; i++) bp_refresh_pins(ac, fg, &fg->nodes[i]);
    uint32_t fentry = fg->nodes[0].id, fret = fg->nodes[1].id;
    uint32_t fmul = node(ac, fg, BPK_MUL, 1, NULL, NULL);
    link(fg, fentry, "", fret, "");
    link(fg, fentry, "X", fmul, "A");
    lit(fg, fmul, "B", 2, 0, 0);
    link(fg, fmul, "", fret, "Result");
    // the Event Graph (the template's BeginPlay -> Print removed: ours instead)
    BpGraph* g = &ac->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t begin = node(ac, g, BPK_EVENT_BEGIN_PLAY, 0, NULL, NULL);
    uint32_t loop = node(ac, g, BPK_FOR_LOOP, 1, NULL, NULL);
    lit(g, loop, "First Index", 1, 0, 0);
    lit(g, loop, "Last Index", 10, 0, 0);
    // Gameplay Tags: BeginPlay -> Add Gameplay Tag(self, State.Stunned) -> Stunned = Has Matching Gameplay Tag(self, State)
    uint32_t tag_add = node(ac, g, BPK_TAG_ADD, 0.3f, NULL, NULL);
    slit(g, tag_add, "Tag", "State.Stunned");
    uint32_t tag_has = node(ac, g, BPK_TAG_HAS, 0.5f, NULL, NULL);
    slit(g, tag_has, "Tag", "State");
    uint32_t set_stunned = node(ac, g, BPK_VAR_SET, 0.6f, "Stunned", NULL);
    link(g, begin, "", tag_add, "");
    link(g, tag_add, "", set_stunned, "");
    link(g, tag_has, "", set_stunned, "Stunned");
    // Format Text: Formatted = Format Text("Speed {0} {{ok}}", Speed)
    uint32_t fmt = node(ac, g, BPK_FORMAT_TEXT, 0.7f, NULL, NULL);
    slit(g, fmt, "Format", "Speed {0} {{ok}}");
    uint32_t get_speed = node(ac, g, BPK_VAR_GET, 0.65f, "Speed", NULL);
    link(g, get_speed, "Speed", fmt, "{0}");
    uint32_t set_fmt = node(ac, g, BPK_VAR_SET, 0.8f, "Formatted", NULL);
    // ... -> TagMatching = Length(Get All Actors with Gameplay Tag(State)) -> TagExact = Length(Get All Actors with Tag(State))
    uint32_t all_g = node(ac, g, BPK_GET_ALL_WITH_GTAG, 0.62f, NULL, NULL);
    slit(g, all_g, "Gameplay Tag", "State");
    uint32_t len_g = node(ac, g, BPK_ARR_LEN, 0.63f, NULL, NULL);
    link(g, all_g, "Out Actors", len_g, "Target Array");
    uint32_t set_tm = node(ac, g, BPK_VAR_SET, 0.64f, "TagMatching", NULL);
    link(g, len_g, "", set_tm, "TagMatching");
    uint32_t all_t = node(ac, g, BPK_GET_ALL_WITH_TAG, 0.66f, NULL, NULL);
    slit(g, all_t, "Tag", "State");
    uint32_t len_t = node(ac, g, BPK_ARR_LEN, 0.67f, NULL, NULL);
    link(g, all_t, "Out Actors", len_t, "Target Array");
    uint32_t set_te = node(ac, g, BPK_VAR_SET, 0.68f, "TagExact", NULL);
    link(g, len_t, "", set_te, "TagExact");
    link(g, set_stunned, "", all_g, "");
    link(g, all_g, "", set_tm, "");
    link(g, set_tm, "", all_t, "");
    link(g, all_t, "", set_te, "");
    link(g, set_te, "", set_fmt, "");
    link(g, fmt, "", set_fmt, "Formatted");
    // Custom Events with inputs: Hit(3) here, then Hit(5) the way another actor's event is called (Target Class, Target)
    uint32_t call_l = node(ac, g, BPK_CALL_CUSTOM, 0.85f, "Hit", NULL);
    lit(g, call_l, "Amount", 3, 0, 0);
    uint32_t call_r = node(ac, g, BPK_CALL_CUSTOM, 0.9f, "Hit", "BP_TestActor");
    lit(g, call_r, "Amount", 5, 0, 0);
    uint32_t self_n = node(ac, g, BPK_SELF, 0.88f, NULL, NULL);
    link(g, self_n, "Self", call_r, "Target");
    link(g, set_fmt, "", call_l, "");
    link(g, call_l, "", call_r, "");
    link(g, call_r, "", loop, "");
    uint32_t ev_hit = node(ac, g, BPK_EVENT_CUSTOM, 20, "Hit", NULL);
    uint32_t get_hit = node(ac, g, BPK_VAR_GET, 21, "LastHit", NULL);
    uint32_t add_hit = node(ac, g, BPK_ADD, 22, NULL, NULL);
    uint32_t set_hit = node(ac, g, BPK_VAR_SET, 23, "LastHit", NULL);
    link(g, ev_hit, "", set_hit, "");
    link(g, get_hit, "LastHit", add_hit, "A");
    link(g, ev_hit, "Amount", add_hit, "B");
    link(g, add_hit, "", set_hit, "LastHit");
    uint32_t get_total = node(ac, g, BPK_VAR_GET, 2, "Total", NULL);
    uint32_t add = node(ac, g, BPK_ADD, 3, NULL, NULL);
    uint32_t set_total = node(ac, g, BPK_VAR_SET, 4, "Total", NULL);
    link(g, loop, "Loop Body", set_total, "");
    link(g, get_total, "Total", add, "A");
    link(g, loop, "Index", add, "B");
    link(g, add, "", set_total, "Total");
    // Completed -> Count = AddTwice(DA_Test.Damage) -> Stats = Make S_Test(5) -> Branch(Count > 15)
    uint32_t call = node(ac, g, BPK_CALL_FUNCTION, 5, "AddTwice", NULL);
    uint32_t dmg = node(ac, g, BPK_DATA_GET, 6, "DA_Test", "Damage");
    uint32_t set_count = node(ac, g, BPK_VAR_SET, 7, "Count", NULL);
    link(g, loop, "Completed", call, "");
    link(g, dmg, "Damage", call, "X");
    link(g, call, "", set_count, "");
    link(g, call, "Result", set_count, "Count");
    uint32_t make = node(ac, g, BPK_MAKE_STRUCT, 8, "S_Test", NULL);
    lit(g, make, "Value", 5, 0, 0);
    uint32_t set_stats = node(ac, g, BPK_VAR_SET, 9, "Stats", NULL);
    link(g, set_count, "", set_stats, "");
    link(g, make, "S_Test", set_stats, "Stats");
    uint32_t get_count = node(ac, g, BPK_VAR_GET, 10, "Count", NULL);
    uint32_t gt = node(ac, g, BPK_GREATER, 11, NULL, NULL);
    link(g, get_count, "Count", gt, "A");
    lit(g, gt, "B", 15, 0, 0);
    uint32_t br = node(ac, g, BPK_BRANCH, 12, NULL, NULL);
    link(g, set_stats, "", br, "");
    link(g, gt, "", br, "Condition");
    // True -> Set Actor Location(Total, Count, Break(Stats).Value * 20) -> Delay 0.1 -> Scale 2 -> Switch(E_Test::1)
    uint32_t get_stats = node(ac, g, BPK_VAR_GET, 13, "Stats", NULL);
    uint32_t brk = node(ac, g, BPK_BREAK_STRUCT, 14, "S_Test", NULL);
    link(g, get_stats, "Stats", brk, "S_Test");
    uint32_t mulz = node(ac, g, BPK_MUL, 15, NULL, NULL);
    link(g, brk, "Value", mulz, "A");
    lit(g, mulz, "B", 20, 0, 0);
    uint32_t mk = node(ac, g, BPK_MAKE_VECTOR, 16, NULL, NULL);
    uint32_t get_total2 = node(ac, g, BPK_VAR_GET, 17, "Total", NULL);
    uint32_t get_count2 = node(ac, g, BPK_VAR_GET, 18, "Count", NULL);
    link(g, get_total2, "Total", mk, "X");
    link(g, get_count2, "Count", mk, "Y");
    link(g, mulz, "", mk, "Z");
    uint32_t setloc = node(ac, g, BPK_SET_LOCATION, 19, NULL, NULL);
    link(g, br, "True", setloc, "");
    link(g, mk, "", setloc, "New Location");
    uint32_t delay = node(ac, g, BPK_DELAY, 20, NULL, NULL);
    lit(g, delay, "Duration", 0.1f, 0, 0);
    link(g, setloc, "", delay, "");
    uint32_t scale = node(ac, g, BPK_SET_SCALE, 21, NULL, NULL);
    lit(g, scale, "New Scale 3D", 2, 2, 2);
    link(g, delay, "Completed", scale, "");
    uint32_t sw = node(ac, g, BPK_SWITCH_ENUM, 22, "E_Test", NULL);
    uint32_t lit_e = node(ac, g, BPK_ENUM_LITERAL, 23, "E_Test", "NewEnumerator1");
    link(g, scale, "", sw, "");
    link(g, lit_e, "", sw, "Selection");
    uint32_t msg = node(ac, g, BPK_CALL_INTERFACE, 24, "BPI_Test", "Interact");
    link(g, sw, "NewEnumerator1", msg, "");
    // Event Interact -> Set Base Color (red)
    uint32_t ev = node(ac, g, BPK_EVENT_INTERFACE, 25, "Interact", NULL);
    uint32_t color = node(ac, g, BPK_SET_COLOR, 26, NULL, NULL);
    lit(g, color, "Color", 1, 0, 0);
    link(g, ev, "", color, "");
    // ... -> Set Post Process Setting (Bloom Intensity 3; no Target: the level's volume)
    uint32_t ppn = node(ac, g, BPK_PP_SET_FLOAT, 31, "Bloom Intensity", NULL);
    lit(g, ppn, "Value", 3, 0, 0);
    link(g, color, "", ppn, "");
    // Tick -> Add Actor Local Rotation(0, Delta * 90, 0)
    uint32_t tick = node(ac, g, BPK_EVENT_TICK, 27, NULL, NULL);
    uint32_t mul = node(ac, g, BPK_MUL, 28, NULL, NULL);
    link(g, tick, "Delta Seconds", mul, "A");
    lit(g, mul, "B", 90, 0, 0);
    uint32_t mr = node(ac, g, BPK_MAKE_ROTATOR, 29, NULL, NULL);
    link(g, mul, "", mr, "Yaw");
    uint32_t rot = node(ac, g, BPK_ADD_ROTATION, 30, NULL, NULL);
    link(g, tick, "", rot, "");
    link(g, mr, "", rot, "Delta Rotation");
    if (g_fail) return false;
    if (!bp_save(ac) || !bp_save(ch)) return false;
    if (!bp_compile(ac) || !bp_compile(ch)) {
        printf("[RealEngine] bp-test: compile failed: %s%s\n", ac->error, ch->error);
        return false;
    }
    printf("[RealEngine] bp-test: BP_TestActor %u ops, %u registers; BP_TestCharacter %u ops\n", ac->ncode, ac->nslots, ch->ncode);
    // Create Child Blueprint Class (the Content Browser's right click): a Character by inheritance, with its parent's body
    if (!bp_editor_create_child("/Content/BpTest/BP_TestCharacter.rbp", false)) {
        printf("[RealEngine] FAIL: bp-test: Create Child Blueprint Class\n");
        return false;
    }
    BpAsset* child = bp_load("/Content/BpTest/BP_TestCharacter_Child.rbp");
    if (!child || strcmp(child->parent, "BP_TestCharacter") || bp_native_of(child) != BPN_CHARACTER ||
        !(child->comp_mask & COMP_BIT(COMP_CHARACTER)) || !bp_compile(child)) {
        printf("[RealEngine] FAIL: bp-test: the child class isn't a BP_TestCharacter\n");
        return false;
    }
    printf("[RealEngine] bp-test: child class %s (parent %s) %u ops\n", child->name, child->parent, child->ncode);
    BpAsset* inp = make_input_class(F);
    if (!inp) return false;
    BpAsset* types = make_types_class(F);
    if (!types) {
        printf("[RealEngine] FAIL: bp-test: the types' class\n");
        return false;
    }
    // the level: a floor, the actor, the character above the floor
    if (ed->bp_test_house) { // inside --scene house: from the dark room through the door into the lit one (GI indoors)
        g_actor = ENTITY_NONE;
        g_char = bp_spawn(&ed->world, ch, (float[3]){ 150, 0, 120 }, (float[3]){ 0, 180, 0 });
    } else {
        // (1 cm under the level's own Floor, at z 0: two planes in one place fought for the depth, and TSR's jitter
        // flipped the winner every frame: the whole floor flickered in some runs, 3-10% against 0.08%)
        Entity floor = editor_spawn_shape(ed, SHAPE_PLANE, (float[3]){ 0, 0, 0 }, 0.0);
        CTransform* ft = ecs_get(&ed->world, floor, COMP_TRANSFORM);
        if (ft) ft->scale[0] = ft->scale[1] = 100.0f, ft->location[2] = -1.0f;
        g_actor = bp_spawn(&ed->world, ac, (float[3]){ 500, 500, 50 }, (float[3]){ 0, 0, 0 });
        CBlueprint* ob = ecs_get(&ed->world, g_actor, COMP_BLUEPRINT);
        if (ob) bp_override_set(ob, "Speed", (float[4]){ 7.0f, 0, 0, 0 }, NULL), bp_override_set(ob, "Label", NULL, "ator");
        g_ppv = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_POST_PROCESS));
        if (g_ppv) post_volume_defaults(ecs_get(&ed->world, g_ppv, COMP_POST_PROCESS));
        g_types = bp_spawn(&ed->world, types, (float[3]){ -500, 500, 50 }, (float[3]){ 0, 0, 0 });
        // (its Blueprint scales it to 42 by the values it checks: without collision, else a 42 m box where it ends up)
        CTransform* tt = g_types ? ecs_get(&ed->world, g_types, COMP_TRANSFORM) : NULL;
        if (tt) tt->flags |= TRANSFORM_NO_COLLISION;
        g_input = bp_spawn(&ed->world, inp, (float[3]){ -800, 500, 50 }, (float[3]){ 0, 0, 0 });
        g_char = bp_spawn(&ed->world, ch, (float[3]){ 0, -400, 300 }, (float[3]){ 0, 0, 0 });
    }
    BpAsset* wb = make_watch_class(F, "BP_TestOverlapBegin", BPK_EVENT_BEGIN_OVERLAP, 2.0f);
    BpAsset* we = make_watch_class(F, "BP_TestOverlapEnd", BPK_EVENT_END_OVERLAP, 3.0f);
    if (!wb || !we) return false;
    BpAsset* dsrc = make_dispatch_source(F);
    BpAsset* dlis = dsrc ? make_dispatch_listener(F) : NULL;
    if (!dlis) return false;
    if (!ed->bp_test_house) {
        g_disp_source = bp_spawn(&ed->world, dsrc, (float[3]){ 300, 900, 50 }, (float[3]){ 0, 0, 0 });
        g_disp_listener = bp_spawn(&ed->world, dlis, (float[3]){ 500, 900, 50 }, (float[3]){ 0, 0, 0 });
    }
    BpAsset* spawner = make_spawn_classes(F);
    if (!spawner) return false;
    if (bp_timeline_selftest() != 0) return false; // (the curves: order, Auto, Linear, Constant, the ends; the speed)
    BpAsset* tlc = make_timeline_class(F, ed->bp_test == 2);
    if (!tlc) return false;
    if (!ed->bp_test_house) g_timeline = bp_spawn(&ed->world, tlc, (float[3]){ 900, 900, 50 }, (float[3]){ 0, 0, 0 });
    if (!ed->bp_test_house) g_spawner = bp_spawn(&ed->world, spawner, (float[3]){ 700, 900, 50 }, (float[3]){ 0, 0, 0 });
    if (!ed->bp_test_house) { // a Trigger Box on the Character's way (it walks along X at y -400)
        g_watch_begin = bp_spawn(&ed->world, wb, (float[3]){ -300, 800, 50 }, (float[3]){ 0, 0, 0 });
        g_watch_end = bp_spawn(&ed->world, we, (float[3]){ -100, 800, 50 }, (float[3]){ 0, 0, 0 });
        g_trigger = ecs_create(&ed->world, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_COLLISION));
        CName* tn = ecs_get(&ed->world, g_trigger, COMP_NAME);
        CTransform* tt = ecs_get(&ed->world, g_trigger, COMP_TRANSFORM);
        CCollision* tc = ecs_get(&ed->world, g_trigger, COMP_COLLISION);
        if (tn) snprintf(tn->text, sizeof tn->text, "TestTrigger");
        if (tt) tt->location[0] = 800.0f, tt->location[1] = -400.0f, tt->location[2] = 100.0f, tt->scale[0] = tt->scale[1] = tt->scale[2] = 1.0f;
        if (tc) tc->shape = COLLISION_BOX, tc->extent[0] = tc->extent[1] = tc->extent[2] = 100.0f, tc->generate_overlap_events = 1, tc->block = 0;
    }
    if (package_dir) { // the packaged game runs them too (ctest game_blueprints)
        char lvl[1100];
        re_make_dirs(package_dir);
        snprintf(lvl, sizeof lvl, "%s/BpLevel.rlevel", package_dir);
        if (!editor_save_level(ed, lvl, 0.0) || !editor_package_game(ed, package_dir, false, 0.0)) {
            printf("[RealEngine] bp-test: packaging into %s failed: %s\n", package_dir, ed->status_msg);
            return false;
        }
    }
    // Level streaming: a sublevel with one cube; the test level saved in Content with a Level Blueprint whose BeginPlay
    // loads it (Load Stream Level): the cube must be in the world while playing
    if (!ed->bp_test_house) {
        static World sub;
        ecs_init(&sub);
        Entity c = ecs_create(&sub, COMP_BIT(COMP_NAME) | COMP_BIT(COMP_TRANSFORM) | COMP_BIT(COMP_STATIC_MESH));
        snprintf(((CName*)ecs_get(&sub, c, COMP_NAME))->text, RE_NAME_MAX, "StreamedCube");
        CTransform* ct = ecs_get(&sub, c, COMP_TRANSFORM);
        ct->location[0] = 800, ct->location[2] = 50, ct->scale[0] = ct->scale[1] = ct->scale[2] = 1;
        ((CStaticMesh*)ecs_get(&sub, c, COMP_STATIC_MESH))->mesh = SHAPE_CUBE;
        char sp[1100], mp[1100];
        snprintf(sp, sizeof sp, "%s/StreamSub.rlevel", dir);
        snprintf(mp, sizeof mp, "%s/BpTestLevel.rlevel", dir);
        LevelCamera scam = { { 0, 0, 200 }, 0, 0, 90, 600 };
        bool ok = level_save(&sub, &scam, sp, NULL) && editor_save_level(ed, mp, 0.0);
        ecs_shutdown(&sub);
        BpAsset* lb = ok ? level_bp_open_or_create(mp) : NULL;
        if (!lb) {
            printf("[RealEngine] FAIL: bp-test: the Level Blueprint could not be made\n");
            return false;
        }
        BpGraph* lg = &lb->graphs[0];
        while (lg->nnodes) bp_remove_node(lg, lg->nodes[0].id);
        uint32_t lbeg = node(lb, lg, BPK_EVENT_BEGIN_PLAY, 0, NULL, NULL);
        uint32_t lld = node(lb, lg, BPK_LOAD_STREAM, 1, NULL, NULL);
        slit(lg, lld, "Level Name", "StreamSub");
        uint32_t lpr = node(lb, lg, BPK_PRINT, 2, NULL, NULL);
        slit(lg, lpr, "In String", "Stream level in");
        link(lg, lbeg, "", lld, "");
        link(lg, lld, "Completed", lpr, "");
        if (g_fail || !bp_save(lb) || !bp_compile(lb)) {
            printf("[RealEngine] FAIL: bp-test: the Level Blueprint didn't compile: %s\n", lb->error);
            return false;
        }
    }
    if (!ed->bp_test) ed->bp_test = 1;
    editor_play(ed, 0.0);
    if (ed->bp_test == 3) editor_set_possessed(ed, false); // the walk seen from a still camera (--bp-walkview)
    return (g_actor || ed->bp_test_house) && g_char && bp_player(&ed->world) == g_char;
}

bool bp_test_check(Editor* ed) {
    World* w = &ed->world;
    bool ok = true;
    if (ed->bp_test_house) { // the walk through the house: it went through the door and stands on the floor
        const CTransform* ht = ecs_get(w, g_char, COMP_TRANSFORM);
        const CCharacter* hc = ecs_get(w, g_char, COMP_CHARACTER);
        if (!ht || !hc) return false;
        printf("[RealEngine] bp-test (house): character at (%.1f, %.1f, %.1f) on ground %u\n", ht->location[0], ht->location[1],
               ht->location[2], hc->on_ground);
        if (!hc->on_ground) printf("[RealEngine] FAIL: bp-test: the Character isn't on the floor\n"), ok = false;
        editor_stop(ed, 0.0);
        return ok;
    }
    const CTransform* t = ecs_get(w, g_actor, COMP_TRANSFORM);
    const CStaticMesh* m = ecs_get(w, g_actor, COMP_STATIC_MESH);
    const CTransform* ct = ecs_get(w, g_char, COMP_TRANSFORM);
    const CCharacter* cc = ecs_get(w, g_char, COMP_CHARACTER);
    if (!t || !m || !ct || !cc) {
        printf("[RealEngine] FAIL: bp-test: the actors are gone\n");
        return false;
    }
    { // (2026-11-16) the Timeline: played to its end (Value 2), its Event key passed (Pings 1), Finished (Done 1)
        BpValue v[4];
        uint32_t wd = 0;
        char text[8];
        bool is_text = false;
        float val = bp_get_instance_var(w, g_timeline, "Value", v, 4, &wd, text, sizeof text, &is_text) ? v[0].v[0] : -1.0f;
        float pings = bp_get_instance_var(w, g_timeline, "Pings", v, 4, &wd, text, sizeof text, &is_text) ? v[0].v[0] : -1.0f;
        float done = bp_get_instance_var(w, g_timeline, "Done", v, 4, &wd, text, sizeof text, &is_text) ? v[0].v[0] : -1.0f;
        printf("[RealEngine] bp-test: Timeline: Value %.3f (want 2), Pings %.0f (want 1), Done %.0f (want 1)\n", val, pings, done);
        if (fabsf(val - 2.0f) > 1e-3f || pings != 1.0f || done != 1.0f) printf("[RealEngine] FAIL: bp-test: the Timeline didn't play\n"), ok = false;
    }
    { // (2026-11-10) Expose on Spawn: the BP_TestSpawned the spawner made has Size 3 at its BeginPlay (its scale 3)
        const BpAsset* sc = bp_find_name(BPA_CLASS, "BP_TestSpawned");
        float got = -1.0f;
        for (uint32_t s2 = 1; sc && s2 < w->slot_count; s2++) {
            Entity e = ecs_entity_at(w, s2);
            const CBlueprint* b = e ? ecs_get(w, e, COMP_BLUEPRINT) : NULL;
            const CTransform* st = e ? ecs_get(w, e, COMP_TRANSFORM) : NULL;
            if (b && st && b->asset_hash == sc->hash) {
                printf("[RealEngine] bp-test: BP_TestSpawned slot %u at (%.0f, %.0f, %.0f) scale %.2f, inst %u\n", s2, st->location[0], st->location[1],
                       st->location[2], st->scale[0], b->inst);
                if (got != 3.0f) got = st->scale[0];
            }
        }
        printf("[RealEngine] bp-test: Expose on Spawn: the spawned actor's scale %.2f (Size 3 on the pin, default 1)\n", got);
        if (fabsf(got - 3.0f) > 0.01f) printf("[RealEngine] FAIL: bp-test: Expose on Spawn didn't give Size before BeginPlay\n"), ok = false;
    }
    { // Instance Editable: the actor's own values over the class's defaults (Speed 1 -> 7, Label "classe" -> "ator")
        BpValue sv[4];
        uint32_t wd = 0;
        char text[64] = "";
        bool is_text = false, ok1 = bp_get_instance_var(w, g_actor, "Speed", sv, 4, &wd, text, sizeof text, &is_text);
        float speed = ok1 ? sv[0].v[0] : -1.0f;
        bool ok2 = bp_get_instance_var(w, g_actor, "Label", sv, 4, &wd, text, sizeof text, &is_text);
        printf("[RealEngine] bp-test: Instance Editable: Speed %.1f, Label \"%s\"\n", speed, ok2 ? text : "?");
        if (!ok1 || fabsf(speed - 7.0f) > 1e-4f || !ok2 || strcmp(text, "ator"))
            printf("[RealEngine] FAIL: bp-test: the actor's Instance Editable values weren't applied (expected 7 and \"ator\")\n"), ok = false;
    }
    { // Custom Events with inputs: Hit(3) jumped to here, Hit(5) run on the Target's instance (OP_CALLEV)
        extern uint32_t g_bp_remote_events;
        BpValue sv[4];
        uint32_t wd = 0;
        char text[8];
        bool is_text = false, got = bp_get_instance_var(w, g_actor, "LastHit", sv, 4, &wd, text, sizeof text, &is_text);
        printf("[RealEngine] bp-test: Custom Event inputs: LastHit %.1f (remote runs %u)%c", got ? sv[0].v[0] : -1.0f, g_bp_remote_events, 10);
        if (!got || fabsf(sv[0].v[0] - 8.0f) > 1e-4f || !g_bp_remote_events)
            printf("[RealEngine] FAIL: bp-test: Custom Event with inputs (expected LastHit 8, a remote run)%c", 10), ok = false;
    }
    { // Format Text: the actor's Speed (7, its Instance Editable value) in the text, {{ a brace
        BpValue sv[4];
        uint32_t wd = 0;
        char text[64] = "";
        bool is_text = false, got = bp_get_instance_var(w, g_actor, "Formatted", sv, 4, &wd, text, sizeof text, &is_text);
        printf("[RealEngine] bp-test: Format Text: [%s]%c", got ? text : "?", 10);
        if (!got || strncmp(text, "Speed 7", 7) || !strstr(text, " {ok}")) printf("[RealEngine] FAIL: bp-test: Format Text (expected Speed 7... {ok})%c", 10), ok = false;
    }
    { // Gameplay Tags: BeginPlay added State.Stunned, Has Matching Gameplay Tag(State) saw it
        BpValue sv[4];
        uint32_t wd = 0;
        char text[8];
        bool is_text = false, got = bp_get_instance_var(w, g_actor, "Stunned", sv, 4, &wd, text, sizeof text, &is_text);
        bool has = gt_has(w, g_actor, "State.Stunned", true);
        printf("[RealEngine] bp-test: Gameplay Tags: tag %s, Stunned %s\n", has ? "on" : "off", got && sv[0].v[0] != 0.0f ? "true" : "false");
        if (!has || !got || sv[0].v[0] == 0.0f) printf("[RealEngine] FAIL: bp-test: Add Gameplay Tag / Has Matching Gameplay Tag\n"), ok = false;
        // the actors with a tag: State matches the actor's State.Stunned (and no other actor's), exactly State none
        BpValue tm[2] = { 0 }, te[2] = { 0 };
        bool g1 = bp_get_instance_var(w, g_actor, "TagMatching", tm, 2, &wd, text, sizeof text, &is_text);
        bool g2 = bp_get_instance_var(w, g_actor, "TagExact", te, 2, &wd, text, sizeof text, &is_text);
        uint32_t want = gt_find_actors(w, "State", false, NULL, 0);
        printf("[RealEngine] bp-test: Get All Actors with Gameplay Tag(State) %.0f (of %u), with Tag(State) %.0f\n", tm[0].v[0], want, te[0].v[0]);
        if (!g1 || !g2 || tm[0].v[0] < 1.0f || (uint32_t)tm[0].v[0] > want || te[0].v[0] != 0.0f)
            printf("[RealEngine] FAIL: bp-test: Get All Actors with Tag / with Gameplay Tag\n"), ok = false;
    }
    { // the Construction Script ran (before BeginPlay): the source turned to yaw 33
        const CTransform* s = ecs_get(w, g_disp_source, COMP_TRANSFORM);
        printf("[RealEngine] bp-test: Construction Script: source yaw %.1f\n", s ? s->rotation[1] : -1.0f);
        if (!s || fabsf(s->rotation[1] - 33.0f) > 0.01f) printf("[RealEngine] FAIL: bp-test: the Construction Script didn't run (expected yaw 33)\n"), ok = false;
    }
    { // the Event Dispatcher: the listener bound to the source's OnPing, called with Amount 4
        const CTransform* l = ecs_get(w, g_disp_listener, COMP_TRANSFORM);
        printf("[RealEngine] bp-test: Event Dispatcher: listener scale %.1f, yaw %.1f (the delegate's event)\n", l ? l->scale[0] : -1.0f, l ? l->rotation[1] : -1.0f);
        if (!l || fabsf(l->rotation[1] - 77.0f) > 0.01f)
            printf("[RealEngine] FAIL: bp-test: the Custom Event bound by its delegate (red square) didn't run (expected yaw 77)\n"), ok = false;
        if (!l || fabsf(l->scale[0] - 4.0f) > 0.01f) {
            extern uint32_t g_bp_dispatch_calls, g_bp_dispatch_runs;
            extern float g_bp_dispatch_arg0;
            printf("[RealEngine] FAIL: bp-test: the bound Event OnPing didn't run (expected scale 4; %u calls, %u bound events run, "
                   "last Amount %.2f)\n", g_bp_dispatch_calls, g_bp_dispatch_runs, g_bp_dispatch_arg0);
            void bp_debug_dump(Entity e, const char* label);
            bp_debug_dump(g_disp_listener, "BP_TestDispListener");
            ok = false;
        }
    }
    { // the Trigger Box: begun and ended as the Character walked through it
        const CTransform* b = ecs_get(w, g_watch_begin, COMP_TRANSFORM);
        const CTransform* e = ecs_get(w, g_watch_end, COMP_TRANSFORM);
        printf("[RealEngine] bp-test: overlaps: begin watcher scale %.1f, end watcher scale %.1f\n", b ? b->scale[0] : -1.0f, e ? e->scale[0] : -1.0f);
        if (ed->bp_test != 2 && (!b || !e || fabsf(b->scale[0] - 2.0f) > 0.01f || fabsf(e->scale[0] - 3.0f) > 0.01f)) // (standing: it never reaches it)
            printf("[RealEngine] FAIL: bp-test: the Trigger Box's ActorBeginOverlap / ActorEndOverlap didn't run (expected 2 and 3)\n"), ok = false;
    }
    printf("[RealEngine] bp-test: actor at (%.1f, %.1f, %.1f) scale %.2f yaw %.1f color (%.2f, %.2f, %.2f); character at (%.1f, %.1f, %.1f) "
           "on ground %u, control yaw %.1f velocity (%.0f, %.0f, %.0f); %u instances, %.4f ms, %u ops last frame\n",
           t->location[0], t->location[1], t->location[2], t->scale[0], t->rotation[1], m->base_color[0], m->base_color[1],
           m->base_color[2], ct->location[0], ct->location[1], ct->location[2], cc->on_ground, cc->control_yaw, cc->velocity[0], cc->velocity[1], cc->velocity[2], bp_stats()->instances,
           bp_stats()->ms_last_frame, bp_stats()->ops_last_frame);
    // For Loop 1..10: 55; Count = AddTwice(Damage 10) = 20; Break(Stats).Value 5 * 20 = 100
    if (fabsf(t->location[0] - 55.0f) > 0.01f || fabsf(t->location[1] - 20.0f) > 0.01f || fabsf(t->location[2] - 100.0f) > 0.01f) {
        printf("[RealEngine] FAIL: bp-test: Set Actor Location (loop, function, data asset, struct) expected (55, 20, 100)\n");
        ok = false;
    }
    if (fabsf(t->scale[0] - 2.0f) > 0.01f) printf("[RealEngine] FAIL: bp-test: the Delay didn't resume\n"), ok = false;
    if (m->base_color[0] < 0.99f || m->base_color[1] > 0.01f) printf("[RealEngine] FAIL: bp-test: the interface message didn't arrive\n"), ok = false;
    const CPostProcess* ppv = g_ppv ? ecs_get(w, g_ppv, COMP_POST_PROCESS) : NULL;
    if (!ppv || fabsf(ppv->s.v[PP_BLOOM_INTENSITY] - 3.0f) > 1e-4f || !(ppv->overrides >> PP_BLOOM_INTENSITY & 1u))
        printf("[RealEngine] FAIL: bp-test: Set Post Process Setting didn't change the volume\n"), ok = false;
    if (fabsf(t->rotation[1]) < 1.0f) printf("[RealEngine] FAIL: bp-test: Event Tick didn't turn the actor\n"), ok = false;
    if (fabsf(ct->location[2] - cc->half_height) > 3.0f || !cc->on_ground)
        printf("[RealEngine] FAIL: bp-test: the Character didn't land on the floor\n"), ok = false;
    if (ed->bp_test != 2 && ct->location[0] < 200.0f) printf("[RealEngine] FAIL: bp-test: the Character didn't walk (MoveForward)\n"), ok = false;
    if (ed->bp_test == 2 && fabsf(ct->location[0]) > 0.01f) printf("[RealEngine] FAIL: bp-test: the Character moved while standing\n"), ok = false;
    const CTransform* tt = g_types ? ecs_get(w, g_types, COMP_TRANSFORM) : NULL;
    if (tt) printf("[RealEngine] bp-test: types actor at (%.1f, %.1f, %.1f) yaw %.1f scale %.1f\n", tt->location[0], tt->location[1],
                   tt->location[2], tt->rotation[1], tt->scale[0]);
    if (!tt || fabsf(tt->location[0] - 10.0f) > 0.01f || fabsf(tt->location[1] - 4.0f) > 0.01f || fabsf(tt->location[2] - 14.0f) > 0.01f ||
        fabsf(tt->rotation[1] - 45.0f) > 0.01f)
        printf("[RealEngine] FAIL: bp-test: arrays / strings / split transform / macro expected (10, 4, 14) yaw 45\n"), ok = false;
    if (!tt || fabsf(tt->scale[0] - 42.0f) > 0.01f) printf("[RealEngine] FAIL: bp-test: the interface function's output expected 42\n"), ok = false;
    const CTransform* it = g_input ? ecs_get(w, g_input, COMP_TRANSFORM) : NULL; // (pawn_input holds W in the test; --bp-idle holds nothing)
    printf("[RealEngine] bp-test: Enhanced Input scale z %.2f\n", it ? it->scale[2] : -1.0f);
    if (ed->bp_test == 1 && (!it || fabsf(it->scale[2] - 3.0f) > 0.01f)) {
        float iv[3] = { 0 }, el = 0.0f, tr = 0.0f;
        uint32_t ev = 0;
        bool known = input_action_state("IA_Move", iv, &ev, &el, &tr);
        printf("[RealEngine] FAIL: bp-test: IA_Move (W through IMC_Default) expected scale z 3 (IA_Move %s: value %.2f %.2f, events %x, "
               "triggered %.2f s; W held %d, possessed %d, released %d, player %u)\n", known ? "known" : "unknown", iv[0], iv[1], ev, tr,
               bp_key_held('W'), ed->possessed, ed->mouse_released, bp_player(w));
        void bp_debug_dump(Entity e, const char* label);
        bp_debug_dump(g_input, "BP_TestInput");
        int ialive, iinputs, ii = bp_debug_instance(g_input, &ialive, &iinputs);
        extern uint32_t g_bp_input_runs;
        printf("[RealEngine] bp-test: BP_TestInput instance %d alive %d, %d input events (%u runs in all); scale %.2f %.2f %.2f\n", ii, ialive, iinputs,
               g_bp_input_runs, it ? it->scale[0] : -1.0f, it ? it->scale[1] : -1.0f, it ? it->scale[2] : -1.0f);
        ok = false;
    }
    bool streamed = false; // the Level Blueprint's Load Stream Level brought the sublevel's cube
    for (uint32_t i = 1; i < w->slot_count && !streamed; i++) {
        Entity e = ecs_entity_at(w, i);
        const CName* n = e ? ecs_get(w, e, COMP_NAME) : NULL;
        streamed = n && !strcmp(n->text, "StreamedCube") && ecs_get(w, e, COMP_STREAMED);
    }
    printf("[RealEngine] bp-test: level streaming %s\n", streamed ? "in" : "missing");
    if (!streamed) printf("[RealEngine] FAIL: bp-test: Load Stream Level (the Level Blueprint) didn't bring the sublevel in\n"), ok = false;
    editor_stop(ed, 0.0);
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/BpTest", content_root());
    if (!getenv("REALENGINE_BP_KEEP")) re_remove_tree(dir); // kept: to look at them in the editor
    return ok;
}

// The Construction Script in the editor (sugestão #64, editor_construct.c): BP_TestDispSource turns itself to yaw 33 in
// its Construction Script. Placed in the editor (no Play): it shows turned; moved, it stays turned and keeps the move;
// before a save (editor_construct_restore) it is back as authored (yaw 0, the move kept); the next frame turned again.
// Its cost measured (a script of one node: microseconds).
bool bp_test_construct_editor(Editor* ed) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/BpTestCS", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    BpAsset* cls = make_dispatch_source("/Content/BpTestCS");
    if (!cls) return false;
    Entity e = bp_spawn(&ed->world, cls, (float[3]){ 100, 200, 50 }, (float[3]){ 0, 0, 0 });
    CTransform* t = e ? ecs_get(&ed->world, e, COMP_TRANSFORM) : NULL;
    bool ok = t != NULL;
    editor_construct_update(ed); // (a frame)
    t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    bool turned = ok && fabsf(t->rotation[1] - 33.0f) < 0.01f;
    double ms = editor_construct_ms();
    if (ok) t->location[0] += 150.0f; // the user moves it
    editor_construct_update(ed);
    t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    bool moved = ok && fabsf(t->location[0] - 250.0f) < 0.01f && fabsf(t->rotation[1] - 33.0f) < 0.01f;
    editor_construct_restore(ed); // (before a save, Play, an undo step)
    t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    bool authored = ok && fabsf(t->location[0] - 250.0f) < 0.01f && fabsf(t->rotation[1]) < 0.01f;
    editor_construct_update(ed);
    t = ecs_get(&ed->world, e, COMP_TRANSFORM);
    bool again = ok && fabsf(t->rotation[1] - 33.0f) < 0.01f;
    editor_construct_update(ed); // nothing changed: nothing runs
    bool idle = editor_construct_ms() == 0.0;
    printf("[RealEngine] Construction Script (editor): turned %d, moved %d, authored on save %d, again %d, idle %d, %.3f ms\n", turned, moved,
           authored, again, idle, ms);
    ecs_destroy(&ed->world, e);
    editor_construct_forget();
    re_remove_tree(dir); // (its class out of the project: another test's BP_TestDispSource keeps the name)
    bool pass = turned && moved && authored && again && idle;
    if (!pass) printf("[RealEngine] FAIL: the Construction Script in the editor\n");
    return pass;
}
