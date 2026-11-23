// The Blueprint debugger's test (realengine --bp-debug-test, ctest "bp_debugger"; bp_debug.h, notas-dev/blueprints.md
// "Debugger"). A class made through the editor's API: BeginPlay -> Set Total = 5 -> Call AddTwice(Total) -> Set Total =
// Result -> Print; Tick -> Count = Count + 1. A Real script: BeginPlay sets n = 1, then n = n + 41, then prints.
// Played: the breakpoint on Call AddTwice stops BeginPlay (the world waits: Count stays 0 for 20 frames); the pin X and
// the variable Total read 5; Step Into stops in the function (2 frames on the stack); Step Over comes back to Set Total
// (Result 10), again to the Print (Total 10); Continue; the script's breakpoint on its line 6 stops there (n 1); a Step
// Over goes by lines (line 7, n 42); Continue; a breakpoint on the Tick's Set stops a frame (its wire traced), removed,
// Continue: the game goes on. Then the cost: the same loop with no breakpoint and with one elsewhere in its class,
// interpreted and native: a breakpoint costs nothing where it isn't.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bp_debug.h"
#include "bp_internal.h"
#include "components.h"
#include "content.h"
#include "editor.h"
#include "platform.h"
#include "platform_files.h"
#include "script.h"

static bool g_fail;
static int g_phase, g_wait, g_pf; // (g_pf: the frames paused in this phase)
static int g_bt, g_st;
static Entity g_bp_actor, g_script_actor;
static BpAsset* g_cls;
static ScriptFile* g_sf;
static uint32_t g_n_call, g_n_set2, g_n_print, g_n_tick_set, g_fgraph;
static float g_count_at_pause;
static bool g_done;

#define FAIL(...) (printf("[RealEngine] FAIL: bp-debug: " __VA_ARGS__), printf("\n"), g_fail = true)

static int pin(BpGraph* g, uint32_t id, int out, const char* name) {
    BpNode* n = bp_node(g, id);
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name)) return i;
    FAIL("node %u has no pin %s", id, name);
    return 0;
}
static void link(BpGraph* g, uint32_t a, const char* ap, uint32_t b, const char* bpn) {
    if (!bp_link(g, a, (uint8_t)pin(g, a, 1, ap), b, (uint8_t)pin(g, b, 0, bpn))) FAIL("link %u.%s -> %u.%s refused", a, ap, b, bpn);
}
static uint32_t node(BpAsset* a, BpGraph* g, uint16_t kind, float x, const char* text) {
    BpNode* n = bp_add_node(a, g, kind, fmodf(x, 6.0f) * 300.0f, floorf(x / 6.0f) * 190.0f, text, NULL);
    if (!n) g_fail = true;
    return n ? n->id : 0;
}
static void lit(BpGraph* g, uint32_t id, const char* name, float x) {
    BpNode* n = bp_node(g, id);
    int p = pin(g, id, 0, name);
    if (n) n->pins[p].lit = (BpValue){ { x, 0, 0, 0 } };
}

static const char* const g_script =
    "class DbgScript extends Actor\n"   // 1
    "    var n = 0\n"                   // 2
    "\n"                                // 3
    "    event BeginPlay()\n"           // 4
    "        n = 1\n"                   // 5
    "        n = n + 41\n"              // 6
    "        print(\"script {n}\")\n";  // 7

static const char* const g_bench =
    "class DbgBench extends Actor\n"
    "    var total = 0.0\n"
    "\n"
    "    event Run()\n"
    "        var x = 0.0\n"
    "        var acc = 0.0\n"
    "        for i in 0..40000\n"
    "            x = x * 0.5 + i * 2.0 - 1.0\n"
    "            if x > 1000.0\n"
    "                x = x - 1000.0\n"
    "            acc += x\n"
    "        total = acc\n"
    "\n"
    "    event Other()\n"
    "        total = 1.0\n";                // (line 15: a breakpoint here is off Run's path)

static bool write_file(const char* path, const char* text) {
    FILE* f = re_fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    fclose(f);
    return ok;
}

bool bp_debug_test_setup(Editor* ed) {
    char dir[1024], path[1200];
    snprintf(dir, sizeof dir, "%s/BpDebugTest", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    const char* F = "/Content/BpDebugTest";
    // (a name of this run's: the registry may still hold a class of a run before, read when the editor started)
    char cname[48];
    snprintf(cname, sizeof cname, "BP_DbgActor%u", (unsigned)(re_time_seconds() * 1000.0) % 1000000u);
    BpAsset* a = bp_create(BPA_CLASS, F, cname, "Actor");
    if (!a) return printf("[RealEngine] FAIL: bp-debug: the class\n"), false;
    BpVar* v = &a->vars[a->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "Total"), v->type = BPT_FLOAT;
    v = &a->vars[a->nvars++];
    memset(v, 0, sizeof *v), snprintf(v->name, BP_NAME, "Count"), v->type = BPT_FLOAT;
    // AddTwice(X) -> Result = X * 2
    bp_add_function(a, "AddTwice");
    g_fgraph = a->ngraphs - 1;
    BpGraph* fg = &a->graphs[g_fgraph];
    fg->nparams = 1, fg->nresults = 1;
    memset(&fg->params[0], 0, sizeof fg->params[0]), memset(&fg->results[0], 0, sizeof fg->results[0]);
    snprintf(fg->params[0].name, BP_NAME, "X"), fg->params[0].type = BPT_FLOAT;
    snprintf(fg->results[0].name, BP_NAME, "Result"), fg->results[0].type = BPT_FLOAT;
    for (uint32_t i = 0; i < fg->nnodes; i++) bp_refresh_pins(a, fg, &fg->nodes[i]);
    uint32_t fentry = fg->nodes[0].id, fret = fg->nodes[1].id;
    uint32_t fmul = node(a, fg, BPK_MUL, 1, NULL);
    link(fg, fentry, "", fret, "");
    link(fg, fentry, "X", fmul, "A");
    lit(fg, fmul, "B", 2);
    link(fg, fmul, "", fret, "Result");
    BpGraph* g = &a->graphs[0];
    while (g->nnodes) bp_remove_node(g, g->nodes[0].id);
    uint32_t begin = node(a, g, BPK_EVENT_BEGIN_PLAY, 0, NULL);
    uint32_t set1 = node(a, g, BPK_VAR_SET, 1, "Total");
    uint32_t five = node(a, g, BPK_ADD, 7, NULL); // (2 + 3: a pure node's value into the Set)
    lit(g, five, "A", 2), lit(g, five, "B", 3);
    link(g, five, "", set1, "Total");
    g_n_call = node(a, g, BPK_CALL_FUNCTION, 2, "AddTwice");
    uint32_t get_total = node(a, g, BPK_VAR_GET, 8, "Total");
    g_n_set2 = node(a, g, BPK_VAR_SET, 3, "Total");
    g_n_print = node(a, g, BPK_PRINT, 4, NULL);
    link(g, begin, "", set1, "");
    link(g, set1, "", g_n_call, "");
    link(g, get_total, "Total", g_n_call, "X");
    link(g, g_n_call, "", g_n_set2, "");
    link(g, g_n_call, "Result", g_n_set2, "Total");
    link(g, g_n_set2, "", g_n_print, "");
    uint32_t tick = node(a, g, BPK_EVENT_TICK, 12, NULL);
    uint32_t get_count = node(a, g, BPK_VAR_GET, 13, "Count");
    uint32_t add = node(a, g, BPK_ADD, 14, NULL);
    g_n_tick_set = node(a, g, BPK_VAR_SET, 15, "Count");
    lit(g, add, "B", 1);
    link(g, tick, "", g_n_tick_set, "");
    link(g, get_count, "Count", add, "A");
    link(g, add, "", g_n_tick_set, "Count");
    if (g_fail || !bp_save(a)) return printf("[RealEngine] FAIL: bp-debug: save\n"), false;
    // (the registry's: the first look up scans the Content folder, and the Play and the editor use what it holds)
    BpAsset* reg = bp_find_hash(a->hash);
    if (reg) a = reg;
    if (!bp_compile(a)) return printf("[RealEngine] FAIL: bp-debug: compile: %s\n", a->error), false;
    g_cls = a;
    printf("[RealEngine] bp-debug: %s %u ops, %u debugger nodes, %u node starts\n", a->name, a->ncode, a->ndbg_nodes, a->ndbg_sites);
    // the script
    snprintf(path, sizeof path, "%s/DbgScript.real", dir);
    if (!write_file(path, g_script)) return false;
    g_sf = script_load("/Content/BpDebugTest/DbgScript.real");
    if (!g_sf || !g_sf->ok || !g_sf->cls) return printf("[RealEngine] FAIL: bp-debug: the script didn't compile\n"), false;
    // the breakpoints: on Call AddTwice and on the script's line 6
    if (!bp_debug_toggle(a, 0, g_n_call)) FAIL("the breakpoint wasn't set");
    if (bp_debug_toggle_line(g_sf->cls, 6) != 1 || !bp_debug_line_has(g_sf->cls, 6)) FAIL("the script's line 6 has no breakpoint");
    printf("[RealEngine] bp-debug: %u ops replaced by OP_BREAK (class compiled %d, %u starts; registry's %d)\n", bp_debug_stats()->patches,
           a->compiled, a->ndbg_sites, bp_find_hash(a->hash) == a);
    g_bp_actor = bp_spawn(&ed->world, a, (float[3]){ 0, 300, 50 }, (float[3]){ 0, 0, 0 });
    g_script_actor = bp_spawn(&ed->world, g_sf->cls, (float[3]){ 0, 600, 50 }, (float[3]){ 0, 0, 0 });
    g_phase = 0, g_wait = 0, g_done = false, g_bt = g_st = 0;
    editor_play(ed, 0.0);
    return !g_fail;
}

static float var_of(Entity e, const char* name) {
    char nm[64], val[128];
    for (uint32_t i = 0; i < 64; i++) {
        nm[0] = 0;
        bool ok = bp_debug_var_text(e, i, nm, sizeof nm, val, sizeof val);
        if (!nm[0]) break;
        if (ok && !strcmp(nm, name)) return (float)atof(val);
    }
    return -999.0f;
}
static float pin_value(uint32_t graph, uint32_t node, int out, const char* name) {
    BpNode* n = bp_node(&g_cls->graphs[graph], node);
    char val[128];
    for (int i = 0; n && i < n->npins; i++)
        if (n->pins[i].out == out && !strcmp(n->pins[i].name, name))
            return bp_debug_pin_text(g_cls, graph, node, i, ENTITY_NONE, val, sizeof val) ? (float)atof(val) : -999.0f;
    return -999.0f;
}
// Paused there: owner's node in graph (line: a script's), else a failure named what
static bool at(const BpAsset* owner, uint32_t graph, uint32_t node, int line, const char* what) {
    BpDbgFrame f[8];
    uint32_t n = bp_debug_stack(f, 8);
    if (!bp_debug_paused() || !n) return FAIL("%s: not paused", what), false;
    bool ok = f[0].owner == owner && (line ? f[0].line == line : f[0].graph == graph && f[0].node_id == node);
    printf("[RealEngine] bp-debug: %s: paused at %s (%u frames)%s\n", what, f[0].what, n, ok ? "" : " - not the expected node");
    for (uint32_t i = 1; i < n; i++) printf("[RealEngine] bp-debug:     %s\n", f[i].what);
    if (!ok) g_fail = true;
    return ok;
}

// The BeginPlays' pauses, whichever comes first (the actors' order): the class's steps (bt) and the script's (st)
static void begin_plays(BpValue cnt) {
    BpDbgFrame f;
    if (!bp_debug_paused() || !bp_debug_stack(&f, 1) || g_pf < 2) return;
    if (f.owner == g_sf->cls) {
        if (g_st == 0) { // the script's line 6, n 1
            if (at(g_sf->cls, 0, 0, 6, "script breakpoint (line 6)")) {
                float n = var_of(bp_debug_paused_actor(), "n");
                printf("[RealEngine] bp-debug: script: n = %.0f, paused line %d\n", n, bp_debug_paused_line(g_sf->cls));
                if (n != 1.0f || bp_debug_paused_line(g_sf->cls) != 6) FAIL("the script's state at line 6");
            }
            bp_debug_command(BPD_STEP_OVER);
        } else if (g_st == 1) { // a step goes by lines: 7, n 42
            if (at(g_sf->cls, 0, 0, 7, "script Step Over (line 7)")) {
                float n = var_of(bp_debug_paused_actor(), "n");
                if (n != 42.0f) FAIL("n %.0f at line 7 (expected 42)", n);
            }
            bp_debug_command(BPD_CONTINUE);
        } else {
            FAIL("the script paused again");
            bp_debug_command(BPD_CONTINUE);
        }
        g_st++, g_pf = 0;
        return;
    }
    switch (g_bt) {
    case 0: // BeginPlay stopped at Call AddTwice; the world waits 20 frames
        if (g_pf == 2) g_count_at_pause = cnt.v[0];
        if (g_pf < 20 || getenv("REALENGINE_BP_DEBUG_HOLD")) return; // (HOLD: it stays paused there, a picture of the debugger)
        if (at(g_cls, 0, g_n_call, 0, "breakpoint")) {
            if (cnt.v[0] != g_count_at_pause || cnt.v[0] != 0.0f) FAIL("the world didn't wait: Count %.0f", cnt.v[0]);
            float x = pin_value(0, g_n_call, 0, "X"), t = var_of(bp_debug_paused_actor(), "Total");
            printf("[RealEngine] bp-debug: inspect: pin X = %.1f, variable Total = %.1f, Count %.0f after 20 paused frames\n", x, t, cnt.v[0]);
            if (x != 5.0f || t != 5.0f) FAIL("the values read %.1f / %.1f (expected 5)", x, t);
        }
        bp_debug_command(BPD_STEP_INTO);
        break;
    case 1: { // in the function
        BpDbgFrame fr[8];
        uint32_t n = bp_debug_stack(fr, 8);
        printf("[RealEngine] bp-debug: Step Into: paused at %s (%u frames)\n", n ? fr[0].what : "?", n);
        for (uint32_t i = 1; i < n; i++) printf("[RealEngine] bp-debug:     %s\n", fr[i].what);
        if (!n || fr[0].graph != g_fgraph || n < 2 || fr[1].node_id != g_n_call) FAIL("Step Into didn't stop in AddTwice under the call");
        bp_debug_command(BPD_STEP_OVER);
        break;
    }
    case 2: // back: Set Total, the call's Result 10
        if (at(g_cls, 0, g_n_set2, 0, "Step Over (out of the function)")) {
            float r = pin_value(0, g_n_call, 1, "Result");
            printf("[RealEngine] bp-debug: inspect: Result = %.1f\n", r);
            if (r != 10.0f) FAIL("Result %.1f (expected 10)", r);
        }
        bp_debug_command(BPD_STEP_OVER);
        break;
    case 3: // the Print, Total 10
        if (at(g_cls, 0, g_n_print, 0, "Step Over")) {
            float t = var_of(bp_debug_paused_actor(), "Total");
            if (t != 10.0f) FAIL("Total %.1f after the step (expected 10)", t);
        }
        bp_debug_command(BPD_CONTINUE);
        break;
    default:
        FAIL("the class paused again in its BeginPlay");
        bp_debug_command(BPD_CONTINUE);
        break;
    }
    g_bt++, g_pf = 0;
}

// Every frame (main.c; also while paused, when the editor's play step doesn't run)
void bp_debug_test_frame(Editor* ed) {
    if (g_done || !ed->playing) return;
    World* w = &ed->world;
    BpValue cnt = { { 0 } };
    bp_instance_var(w, g_bp_actor, "Count", &cnt);
    if (bp_debug_paused()) g_pf++;
    if (++g_wait > 600) {
        FAIL("phase %d didn't happen in 600 frames", g_phase);
        g_done = true;
        return;
    }
    switch (g_phase) {
    case 0: // the BeginPlays' pauses (both done: the game runs)
        begin_plays(cnt);
        if (g_bt == 4 && g_st == 2 && !bp_debug_paused()) g_phase = 6, g_wait = g_pf = 0;
        else if (g_wait > 500) FAIL("the BeginPlays' steps: class %d of 4, script %d of 2", g_bt, g_st), g_done = true;
        break;
    case 6: // running: the Ticks count; a breakpoint on the Tick's Set
        if (bp_debug_paused()) {
            FAIL("paused again after Continue");
            bp_debug_command(BPD_CONTINUE);
            break;
        }
        if (g_wait < 15) break;
        if (cnt.v[0] < 5.0f) FAIL("the game didn't go on after Continue (Count %.0f)", cnt.v[0]);
        bp_debug_watch(g_cls, true); // (the wires that run: as the open editor does)
        bp_debug_toggle(g_cls, 0, g_n_tick_set);
        g_phase++, g_wait = g_pf = 0;
        break;
    case 7: // stopped in a Tick: its wire just ran; the breakpoint removed, Continue
        if (!bp_debug_paused()) break;
        if (g_pf == 1) g_count_at_pause = cnt.v[0];
        if (g_pf < 10) break;
        at(g_cls, 0, g_n_tick_set, 0, "Tick breakpoint");
        if (cnt.v[0] != g_count_at_pause) FAIL("the Ticks went on while paused");
        {
            float age = bp_debug_node_age(g_cls, 0, g_n_tick_set, 0);
            printf("[RealEngine] bp-debug: the Tick's wire ran %.3f s ago (traced)\n", age);
            if (age < 0.0f || age > 0.1f) FAIL("the Tick's wire isn't traced (%.3f)", age);
        }
        bp_debug_toggle(g_cls, 0, g_n_tick_set);
        bp_debug_command(BPD_CONTINUE), g_phase++, g_wait = g_pf = 0;
        break;
    case 8:
        if (bp_debug_paused()) FAIL("paused without a breakpoint");
        if (g_wait < 15) break;
        printf("[RealEngine] bp-debug: running again: Count %.0f; %u hits, %u stops, %u steps, %u outside the tick, resume %.3f ms\n",
               cnt.v[0], bp_debug_stats()->hits, bp_debug_stats()->stops, bp_debug_stats()->steps, bp_debug_stats()->outside,
               bp_debug_stats()->resume_ms);
        g_phase++, g_done = true;
        break;
    }
}

// The VM's cost: the same 40000-turn loop with no breakpoint and with one elsewhere in the class (line 15: Other),
// interpreted and native, the best of 7 each
static bool cost(Editor* ed) {
    char path[1200];
    snprintf(path, sizeof path, "%s/BpDebugTest/DbgBench.real", content_root());
    if (!write_file(path, g_bench)) return false;
    ScriptFile* sf = script_load("/Content/BpDebugTest/DbgBench.real");
    if (!sf || !sf->ok || !sf->cls) return printf("[RealEngine] FAIL: bp-debug: DbgBench didn't compile\n"), false;
    extern bool g_bp_jit;
    bool keep = g_bp_jit;
    double ms[2][2] = { { 1e9, 1e9 }, { 1e9, 1e9 } }; // [breakpoint elsewhere][native]
    float res[2][2] = { { 0 } };
    bool on = false;
    for (int rep = 0; rep < 15; rep++) // (alternated: the breakpoint on and off each turn, the best of 15 of each)
        for (int b = 0; b < 2; b++) {
            if ((b == 1) != on) {
                if (bp_debug_toggle_line(sf->cls, 15) != (b == 1 ? 1u : 0u)) return printf("[RealEngine] FAIL: bp-debug: the breakpoint on line 15\n"), false;
                on = b == 1;
                BpValue w0 = { { 0 } }; // (a turn unmeasured: the native code made again for the changed code)
                g_bp_jit = keep;
                bp_run_event_read(&ed->world, sf->cls, "Run", "total", &w0);
            }
            for (int nat = 0; nat < 2; nat++) {
                g_bp_jit = nat && keep;
                BpValue v = { { 0 } };
                double t0 = re_time_seconds();
                bp_run_event_read(&ed->world, sf->cls, "Run", "total", &v);
                double t = (re_time_seconds() - t0) * 1000.0;
                if (t < ms[b][nat]) ms[b][nat] = t;
                res[b][nat] = v.v[0];
            }
        }
    g_bp_jit = keep;
    if (on) bp_debug_toggle_line(sf->cls, 15);
    printf("[RealEngine] bp-debug: VM cost (40000 turns): no breakpoint %.3f ms interpreted, %.3f ms native; a breakpoint "
           "elsewhere in the class %.3f / %.3f ms\n", ms[0][0], ms[0][1], ms[1][0], ms[1][1]);
    bool ok = res[0][0] == res[1][0] && res[0][1] == res[1][1] && res[0][0] == res[0][1];
    if (!ok) printf("[RealEngine] FAIL: bp-debug: the loop's result changed (%.3f %.3f %.3f %.3f)\n", res[0][0], res[0][1], res[1][0], res[1][1]);
    // (a breakpoint off the path changes nothing: within the measure's noise)
    if (ms[1][0] > ms[0][0] * 1.25 + 0.05 || ms[1][1] > ms[0][1] * 1.25 + 0.05)
        printf("[RealEngine] FAIL: bp-debug: a breakpoint elsewhere made the loop slower\n"), ok = false;
    return ok;
}

bool bp_debug_test_check(Editor* ed) {
    bool ok = !g_fail && g_done;
    if (!g_done) printf("[RealEngine] FAIL: bp-debug: the steps didn't all run (phase %d)\n", g_phase);
    if (bp_debug_paused()) printf("[RealEngine] FAIL: bp-debug: still paused at the end\n"), ok = false;
    editor_stop(ed, 0.0); // (the traces off with the Play)
    bp_debug_clear(NULL);
    if (bp_debug_stats()->patches) printf("[RealEngine] FAIL: bp-debug: %u ops still replaced\n", bp_debug_stats()->patches), ok = false;
    if (!cost(ed)) ok = false;
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/BpDebugTest", content_root());
    re_remove_tree(dir); // (nothing left for the next start to read)
    printf("[RealEngine] bp-debug test: %s\n", ok ? "OK" : "FAIL");
    return ok;
}
