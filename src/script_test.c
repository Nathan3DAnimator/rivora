// --script-test (notas-dev/scripts.md): the same class written in Real, C#, Python and C — a range loop, a while with
// break, a function with a parameter and a return, a Custom Event, string interpolation, the engine's API, Tick — each
// compiled here, an actor of each placed, Play started; the check: every language's variables as the code left them,
// and a script with errors reports them on their lines.
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "blueprint.h"
#include "bp_jit.h"
#include "components.h"
#include "content.h"
#include "editor.h"
#include "platform.h"
#include "platform_files.h"
#include "script.h"
#include "script_ide.h"

static const char* const g_src[SL_COUNT] = {
    // Real
    "struct HitReal\n"
    "    var damage: int = 0\n"
    "    var scale = 1.0\n"
    "\n"
    "class TestReal extends Actor\n"
    "    var sum: int = 0\n"
    "    var doubled = 0\n"
    "    var ticks = 0\n"
    "    var pinged = false\n"
    "    var label = \"\"\n"
    "    var listsum = 0\n"
    "    var picked = 0\n"
    "    var hitsum = 0\n"
    "    var last: HitReal\n"
    "    var next_level: Level = \"StreamSub\"\n"
    "\n"
    "    event BeginPlay()\n"
    "        for i in 0..10\n"
    "            sum += i\n"
    "        doubled = Double(7)\n"
    "        var xs = [3, 4, 5]\n"
    "        for x in xs\n"
    "            listsum += x\n"
    "        match doubled\n"
    "            case 7\n"
    "                picked = 1\n"
    "            case 13, 14\n"
    "                picked = 2\n"
    "            else\n"
    "                picked = 3\n"
    "        var h = HitReal(5, 2.0)\n"
    "        h.damage += 3\n"
    "        last = h\n"
    "        hitsum = last.damage * int(last.scale)\n"
    "        var n = 0\n"
    "        while true\n"
    "            n += 1\n"
    "            if n >= 5\n"
    "                break\n"
    "        label = \"n={n} sum={sum}\"\n"
    "        Ping()\n"
    "        SetActorLocation(GetActorLocation() + Vector(0, 0, 100))\n"
    "\n"
    "    event Ping()\n"
    "        pinged = true\n"
    "\n"
    "    func Double(x: int) -> int\n"
    "        return x * 2\n"
    "\n"
    "    event Tick(dt: float)\n"
    "        ticks += 1\n",
    // C#
    "using RealEngine;\n"
    "\n"
    "public class TestCs : Actor\n"
    "{\n"
    "    int sum = 0;\n"
    "    int doubled;\n"
    "    int ticks;\n"
    "    bool pinged;\n"
    "    string label = \"\";\n"
    "    int listsum;\n"
    "    int picked;\n"
    "    int hitsum;\n"
    "    struct HitCs { public int damage; public float scale; }\n"
    "    HitCs last;\n"
    "    Level next_level = \"StreamSub\";\n"
    "\n"
    "    void BeginPlay()\n"
    "    {\n"
    "        for (int i = 0; i < 10; i++) { sum += i; }\n"
    "        doubled = Double(7);\n"
    "        int[] xs = { 3, 4, 5 };\n"
    "        foreach (int x in xs) listsum += x;\n"
    "        switch (doubled)\n"
    "        {\n"
    "            case 7: picked = 1; break;\n"
    "            case 13:\n"
    "            case 14: picked = 2; break;\n"
    "            default: picked = 3; break;\n"
    "        }\n"
    "        HitCs h = new HitCs(5, 2.0f);\n"
    "        h.damage += 3;\n"
    "        last = h;\n"
    "        hitsum = last.damage * (int)last.scale;\n"
    "        int n = 0;\n"
    "        do { n++; if (n == 3) { n++; } } while (n < 5);\n" // (Q24: do ... while; 1, 2, 3 -> 4, 5)
    "        label = $\"n={n} sum={sum}\";\n"
    "        Ping();\n"
    "        SetActorLocation(GetActorLocation() + new Vector(0, 0, 100));\n"
    "    }\n"
    "    void Ping() { pinged = true; }\n"
    "    int Double(int x) { return x * 2; }\n"
    "    void Tick(float dt) { ticks++; }\n"
    "}\n",
    // Python
    "from realengine import *\n"
    "from dataclasses import dataclass\n"
    "\n"
    "@dataclass\n"
    "class HitPy:\n"
    "    damage: int = 0\n"
    "    scale: float = 1.0\n"
    "\n"
    "class TestPy(Actor):\n"
    "    sum: int = 0\n"
    "    doubled = 0\n"
    "    ticks = 0\n"
    "    pinged = False\n"
    "    label = \"\"\n"
    "    listsum = 0\n"
    "    picked = 0\n"
    "    hitsum = 0\n"
    "    last: HitPy\n"
    "    next_level: Level = \"StreamSub\"\n"
    "\n"
    "    def begin_play(self):\n"
    "        for i in range(10):\n"
    "            self.sum += i\n"
    "        self.doubled = self.double(7)\n"
    "        xs = [3, 4, 5]\n"
    "        for x in xs:\n"
    "            self.listsum += x\n"
    "        match self.doubled:\n"
    "            case 7:\n"
    "                self.picked = 1\n"
    "            case 13 | 14:\n"
    "                self.picked = 2\n"
    "            case _:\n"
    "                self.picked = 3\n"
    "        h = HitPy(5, 2.0)\n"
    "        h.damage += 3\n"
    "        self.last = h\n"
    "        self.hitsum = self.last.damage * int(self.last.scale)\n"
    "        n = 0\n"
    "        while True:\n"
    "            n += 1\n"
    "            if n >= 5:\n"
    "                break\n"
    "        self.label = f\"n={n} sum={self.sum}\"\n"
    "        self.ping()\n"
    "        set_actor_location(get_actor_location() + Vector(0, 0, 100))\n"
    "\n"
    "    def ping(self):\n"
    "        self.pinged = True\n"
    "\n"
    "    def double(self, x: int) -> int:\n"
    "        return x * 2\n"
    "\n"
    "    def tick(self, dt: float):\n"
    "        self.ticks += 1\n",
    // C
    "// parent: Actor\n"
    "#include \"realengine.h\"\n"
    "\n"
    "int sum = 0;\n"
    "int doubled;\n"
    "int ticks;\n"
    "bool pinged;\n"
    "string label = \"\";\n"
    "int listsum;\n"
    "int picked;\n"
    "int hitsum;\n"
    "typedef struct { int damage; float scale; } HitC;\n"
    "HitC last;\n"
    "Level next_level = \"StreamSub\";\n"
    "\n"
    "int Double(int x) { return x * 2; }\n"
    "void Ping(void) { pinged = true; }\n"
    "\n"
    "void BeginPlay(void)\n"
    "{\n"
    "    for (int i = 0; i < 10; i++) sum += i;\n"
    "    doubled = Double(7);\n"
    "    int xs[] = { 3, 4, 5 };\n"
    "    for (int i = 0; i < 3; i++) listsum += xs[i];\n"
    "    switch (doubled) {\n"
    "    case 7: picked = 1; break;\n"
    "    case 13: case 14: picked = 2; break;\n"
    "    default: picked = 3;\n"
    "    }\n"
    "    HitC h = { 5, 2.0f };\n"
    "    h.damage += 3;\n"
    "    last = h;\n"
    "    hitsum = last.damage * (int)last.scale;\n"
    "    int n = 0;\n"
    "    while (1) { n++; if (n >= 5) break; }\n"
    "    label = \"n=5 sum=45\";\n"
    "    Ping();\n"
    "    SetActorLocation(GetActorLocation() + Vector(0, 0, 100));\n"
    "}\n"
    "\n"
    "void Tick(float dt) { ticks++; }\n",
    // C++ (2026-11-25, item 13): the same class, as a C++ programmer writes it
    "#include \"rivora.hpp\"\n"
    "using namespace std;\n"
    "\n"
    "struct HitCpp { int damage; float scale; };\n"
    "\n"
    "class TestCpp : public Actor {\n"
    "public:\n"
    "    int sum = 0;\n"
    "    int doubled = 0;\n"
    "    int ticks = 0;\n"
    "    bool pinged = false;\n"
    "    std::string label = \"\";\n"
    "    int listsum = 0;\n"
    "    int picked = 0;\n"
    "    int hitsum = 0;\n"
    "    HitCpp last;\n"
    "    Level next_level = \"StreamSub\";\n"
    "\n"
    "    void BeginPlay() override {\n"
    "        for (int i = 0; i < 10; ++i) sum += i;\n"
    "        doubled = Double(7);\n"
    "        std::vector<int> xs = { 3, 4, 5 };\n"
    "        for (const auto& x : xs) listsum += x;\n"
    "        switch (doubled) {\n"
    "        case 7: picked = 1; break;\n"
    "        case 13: case 14: picked = 2; break;\n"
    "        default: picked = 3; break;\n"
    "        }\n"
    "        HitCpp h = { 5, 2.0f };\n"
    "        h.damage += 3;\n"
    "        last = h;\n"
    "        hitsum = last.damage * static_cast<int>(last.scale);\n"
    "        int n = 0;\n"
    "        while (true) { ++n; if (n >= 5) break; }\n"
    "        label = \"n=\" + std::to_string(n) + \" sum=\" + to_string(sum);\n"
    "        this->Ping();\n"
    "        SetActorLocation(GetActorLocation() + FVector(0, 0, 100));\n"
    "    }\n"
    "\n"
    "private:\n"
    "    void Ping() { pinged = true; }\n"
    "    int Double(int x) const { return x * 2; }\n"
    "    void Tick(float dt) override { ticks++; }\n"
    "};\n",
};
static const char* const g_names[SL_COUNT] = { "TestReal", "TestCs", "TestPy", "TestC", "TestCpp" };

static Entity g_actor[SL_COUNT];
static bool jit_measure(Editor* ed);
static float g_z0[SL_COUNT];
static bool g_fail;

static bool write_file(const char* path, const char* text) {
    FILE* f = re_fopen(path, "wb");
    if (!f) return false;
    bool ok = fwrite(text, 1, strlen(text), f) == strlen(text);
    return fclose(f) == 0 && ok;
}

bool script_test_setup(Editor* ed) {
    char dir[1024];
    snprintf(dir, sizeof dir, "%s/ScriptTest", content_root());
    re_remove_tree(dir);
    re_make_dirs(dir);
    for (int l = 0; l < SL_COUNT; l++) {
        char path[1200], vp[300];
        snprintf(path, sizeof path, "%s/%s.%s", dir, g_names[l], g_script_lang_ext[l]);
        snprintf(vp, sizeof vp, "/Content/ScriptTest/%s.%s", g_names[l], g_script_lang_ext[l]);
        if (!write_file(path, g_src[l])) return printf("[RealEngine] FAIL: script-test: can't write %s\n", path), false;
        ScriptFile* f = script_load(vp);
        if (!f || !f->ok || !f->cls) {
            printf("[RealEngine] FAIL: script-test: %s didn't compile\n", g_names[l]);
            for (uint32_t i = 0; f && i < f->ndiags; i++)
                printf("[RealEngine]   %s(%d:%d): %s %s\n", g_names[l], f->diags[i].line, f->diags[i].col, f->diags[i].error ? "error" : "warning", f->diags[i].text);
            g_fail = true;
            continue;
        }
        printf("[RealEngine] Script %s (%s): %u nodes, %u ops, %.2f ms, %u warnings\n", g_names[l], g_script_lang_names[l], f->nodes, f->ops,
               f->compile_ms, f->ndiags);
        { // (2026-11-24, item 11) a field of the type Level: a Level reference variable (Name, its class Level), its value kept
            int lv = -1;
            for (uint32_t k = 0; k < f->cls->nvars; k++)
                if (!strcmp(f->cls->vars[k].name, "next_level")) lv = (int)k;
            const BpVar* v = lv >= 0 ? &f->cls->vars[lv] : NULL;
            printf("[RealEngine] Script %s: next_level is %s (%s) = \"%s\"\n", g_names[l], v ? g_bp_type_names[v->type] : "missing", v ? v->ref : "", v ? v->sdef : "");
            if (!v || v->type != BPT_NAME || strcmp(v->ref, "Level") || strcmp(v->sdef, "StreamSub"))
                printf("[RealEngine] FAIL: script-test: %s's Level field isn't a Level reference with its value\n", g_names[l]), g_fail = true;
        }
        for (uint32_t i = 0; i < f->ndiags; i++) printf("[RealEngine]   warning (%d:%d): %s\n", f->diags[i].line, f->diags[i].col, f->diags[i].text);
        float at[3] = { ed->cam.pos[0] + 400.0f, ed->cam.pos[1] + (float)(l - 2) * 150.0f, 100.0f };
        g_actor[l] = bp_spawn(&ed->world, f->cls, at, (float[3]){ 0, 0, 0 });
        g_z0[l] = at[2];
    }
    // a script with errors: each on its line
    const char* broken = "class Broken extends Actor\n"
                         "    event BeginPlay()\n"
                         "        var x = 1\n"
                         "        NoSuchFunction()\n"
                         "        PlaySound2D(5)\n"
                         "        x = Vector(1, 2, 3) * \"text\"\n";
    char bp_[1200];
    snprintf(bp_, sizeof bp_, "%s/Broken.real", dir);
    write_file(bp_, broken);
    ScriptFile* bf = script_load("/Content/ScriptTest/Broken.real");
    bool l4 = false, l5 = false, l6 = false;
    for (uint32_t i = 0; bf && i < bf->ndiags; i++) {
        l4 |= bf->diags[i].line == 4 && bf->diags[i].error;
        l5 |= bf->diags[i].line == 5 && bf->diags[i].error;
        l6 |= bf->diags[i].line == 6 && bf->diags[i].error;
    }
    printf("[RealEngine] Script Broken: %u messages (lines 4 %s, 5 %s, 6 %s)\n", bf ? bf->ndiags : 0, l4 ? "ok" : "MISSING", l5 ? "ok" : "MISSING",
           l6 ? "ok" : "MISSING");
    for (uint32_t i = 0; bf && i < bf->ndiags; i++) printf("[RealEngine]   Broken(%d:%d): %s\n", bf->diags[i].line, bf->diags[i].col, bf->diags[i].text);
    if (!bf || bf->ok || !(l4 && l5 && l6)) g_fail = true;
    // an Editor Utility (a Real class whose parent is EditorUtility): its tool raises the selected actors, run here
    // (not playing) on the editor's level
    const char* util = "class EUW_Raise extends EditorUtility\n"
                       "    event RaiseSelected()\n"
                       "        for i in 0..GetSelectedActorCount()\n"
                       "            var a = GetSelectedActor(i)\n"
                       "            a.AddActorWorldOffset(Vector(0, 0, 50))\n";
    char up_[1200];
    snprintf(up_, sizeof up_, "%s/EUW_Raise.real", dir);
    write_file(up_, util);
    ScriptFile* uf = script_load("/Content/ScriptTest/EUW_Raise.real");
    const CTransform* t0 = g_actor[0] ? ecs_get(&ed->world, g_actor[0], COMP_TRANSFORM) : NULL;
    float before = t0 ? t0->location[2] : 0.0f;
    editor_select(ed, g_actor[0], SEL_REPLACE);
    editor_utilities_bind(ed);
    bool ran = uf && uf->cls && bp_run_editor_event(&ed->world, uf->cls, "RaiseSelected");
    t0 = g_actor[0] ? ecs_get(&ed->world, g_actor[0], COMP_TRANSFORM) : NULL;
    float after = t0 ? t0->location[2] : 0.0f;
    printf("[RealEngine] Editor Utility EUW_Raise: %s, the selected actor %.1f -> %.1f cm\n", ran ? "ran" : "DIDN'T RUN", before, after);
    for (uint32_t i = 0; uf && i < uf->ndiags; i++) printf("[RealEngine]   EUW_Raise(%d:%d): %s\n", uf->diags[i].line, uf->diags[i].col, uf->diags[i].text);
    if (!ran || fabsf(after - before - 50.0f) > 0.5f) g_fail = true;
    g_z0[0] = after;
    // the IDE, the API reference and the utilities drawn through the measure (their drawing exercised)
    char vp0[300];
    snprintf(vp0, sizeof vp0, "/Content/ScriptTest/%s.%s", g_names[0], g_script_lang_ext[0]);
    script_ide_open(vp0);
    script_api_reference_open("PlaySound2D");
    editor_utilities_open();
    if (!jit_measure(ed)) g_fail = true; // (before the Play: a run outside it)
    editor_play(ed, 0.0);
    return !g_fail;
}

// The native code's measure (bp_jit.c): the same loop interpreted and native, the same result, the time of each
static const char* const g_jit_src =
    "class JitBench extends Actor\n"
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
    "        total = acc\n";

static bool jit_measure(Editor* ed) {
    char dir[1024], path[1200];
    snprintf(dir, sizeof dir, "%s/ScriptTest", content_root());
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s/JitBench.real", dir);
    if (!write_file(path, g_jit_src)) return false;
    ScriptFile* sf = script_load("/Content/ScriptTest/JitBench.real");
    if (!sf || !sf->ok || !sf->cls) {
        printf("[RealEngine] FAIL: JIT measure: JitBench didn't compile\n");
        return false;
    }
    extern bool g_bp_jit;
    double ms[2] = { 1e9, 1e9 };
    BpValue res[2] = { { { 0 } }, { { 0 } } };
    for (int pass = 0; pass < 2; pass++) { // 0 interpreted, 1 native (the best of 5 each)
        bool keep = g_bp_jit;
        g_bp_jit = pass == 1 && keep;
        for (int rep = 0; rep < 5; rep++) {
            double t0 = re_time_seconds();
            bool ran = bp_run_event_read(&ed->world, sf->cls, "Run", "total", &res[pass]);
            double t = (re_time_seconds() - t0) * 1000.0;
            if (!ran) {
                printf("[RealEngine] FAIL: JIT measure: Run didn't run\n");
                g_bp_jit = keep;
                return false;
            }
            if (t < ms[pass]) ms[pass] = t;
        }
        g_bp_jit = keep;
    }
    printf("[RealEngine] JIT measure: 40000 loop turns: interpreted %.3f ms, native %.3f ms (%.1fx); total %.3f / %.3f; "
           "native code: %u classes, %u of %u ops, %u KB, %.2f ms to build\n",
           ms[0], ms[1], ms[1] > 0 ? ms[0] / ms[1] : 0.0, res[0].v[0], res[1].v[0], g_bp_jit_stats.classes, g_bp_jit_stats.native_ops,
           g_bp_jit_stats.ops, g_bp_jit_stats.bytes / 1024u, g_bp_jit_stats.build_ms);
    bool ok = res[0].v[0] == res[1].v[0] && res[0].v[0] != 0.0f;
    if (!ok) printf("[RealEngine] FAIL: JIT measure: native %.6f, interpreted %.6f (must be the same)\n", res[1].v[0], res[0].v[0]);
    if (g_bp_jit && ms[1] > ms[0] * 0.8) printf("[RealEngine] FAIL: JIT measure: the native code isn't faster (%.3f vs %.3f ms)\n", ms[1], ms[0]), ok = false;
    return ok;
}

bool script_test_check(Editor* ed) {
    World* w = &ed->world;
    bool ok = !g_fail;
    for (int l = 0; l < SL_COUNT; l++) {
        BpValue sum = { { 0 } }, dbl = { { 0 } }, ticks = { { 0 } }, ping = { { 0 } }, lsum = { { 0 } }, pick = { { 0 } }, hit = { { 0 } };
        char label[96] = "";
        bool got = bp_instance_var(w, g_actor[l], "sum", &sum);
        bp_instance_var(w, g_actor[l], "doubled", &dbl);
        bp_instance_var(w, g_actor[l], "ticks", &ticks);
        bp_instance_var(w, g_actor[l], "pinged", &ping);
        bp_instance_var_text(w, g_actor[l], "label", label, sizeof label);
        bp_instance_var(w, g_actor[l], "listsum", &lsum); // (an array literal summed: 12; switch / match picked the second case: 2)
        bp_instance_var(w, g_actor[l], "picked", &pick);
        bp_instance_var(w, g_actor[l], "hitsum", &hit); // (a structure: made, a field changed, kept in a field, read: 16)
        const CTransform* t = ecs_get(w, g_actor[l], COMP_TRANSFORM);
        float dz = t ? t->location[2] - g_z0[l] : -1.0f;
        bool good = got && sum.v[0] == 45.0f && dbl.v[0] == 14.0f && ticks.v[0] > 10.0f && ping.v[0] != 0.0f && !strcmp(label, "n=5 sum=45") &&
                    fabsf(dz - 100.0f) < 0.5f && lsum.v[0] == 12.0f && pick.v[0] == 2.0f && hit.v[0] == 16.0f;
        printf("[RealEngine] Script %-8s (%s): sum %.0f, doubled %.0f, ticks %.0f, pinged %.0f, label \"%s\", moved %.1f cm, list %.0f, case %.0f, struct %.0f: %s\n",
               g_names[l], g_script_lang_names[l], sum.v[0], dbl.v[0], ticks.v[0], ping.v[0], label, dz, lsum.v[0], pick.v[0], hit.v[0], good ? "ok" : "WRONG");
        ok &= good;
    }
    if (!ok) {
        printf("[RealEngine] FAIL: Scripts: a language's class didn't do what its code says\n");
        return false;
    }
    printf("[RealEngine] Scripts OK\n");
    return true;
}

// --ide-demo: a door script in Real (one line being typed: its completion open; one error), the API reference beside
bool script_ide_demo_setup(Editor* ed) {
    (void)ed;
    char dir[1024], path[1200];
    snprintf(dir, sizeof dir, "%s/Demo", content_root());
    re_make_dirs(dir);
    snprintf(path, sizeof path, "%s/DemoDoor.real", dir);
    const char* src = "class DemoDoor extends Actor\n"
                      "    @editable\n"
                      "    var open_speed: float = 90\n"
                      "    var is_open = false\n"
                      "    var angle = 0.0\n"
                      "\n"
                      "    event BeginPlay()\n"
                      "        print(\"Door ready at {GetActorLocation()}\")\n"
                      "        PlaySound2D(\"Door_Creak\", 0.8, 1.0, 0.0)\n"
                      "\n"
                      "    event Tick(dt: float)\n"
                      "        if is_open and angle < 90\n"
                      "            angle += open_speed * dt\n"
                      "            SetActorRotation(Rotator(0, angle, 0))\n"
                      "        elif not is_open and angle > 0\n"
                      "            angle -= open_speed * dt\n"
                      "\n"
                      "    event Toggle()\n"
                      "        is_open = not is_open\n"
                      "        PlaySound\n"
                      "\n"
                      "    func DoorHeight() -> float\n"
                      "        return GetActorScale3D().z * 200\n";
    if (!write_file(path, src)) return false;
    script_load("/Content/Demo/DemoDoor.real");
    script_ide_demo("/Content/Demo/DemoDoor.real", 20, 18, "PlaySound");
    script_api_reference_open("PlaySound2D");
    return true;
}

