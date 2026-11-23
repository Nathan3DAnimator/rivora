// The Blueprint debugger (Unreal's, notas-dev/blueprints.md "Debugger"): breakpoints on nodes (F9), the Play paused at
// one with the world waiting, the node highlighted, Step Over / Into / Out and Continue (F10, F11, Shift+F11, F5), the
// pins' and variables' values, the call stack, the instance to debug, the wires that ran.
//
// Zero cost without breakpoints: the compiler maps each op to its node (BpAsset.op_src) and each node's start (a "site");
// a breakpoint replaces the op at its sites by OP_BREAK (the original kept aside), so the VM checks nothing per op.
// Paused, the VM's stack stays where it stopped: while any breakpoint is set the Play's ticks run in a fiber of their own
// (bp_fiber.c) that a breakpoint switches out of; the editor goes on drawing, the world waits, Continue switches back.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ecs.h"

struct BpAsset;

// What the compiler keeps per class (bp_vm.c bp_compile): the nodes its ops came from (their graph in their owner, a
// parent's for an inherited graph), where each executable node starts, the variables' registers
typedef struct BpDbgNode {
    const struct BpAsset* owner; // the class whose graph holds the node (the class itself or a parent)
    uint32_t node_id;
    uint16_t graph, kind;
    uint16_t out_reg;            // its first output's register
    int32_t line;                // a script's node: its source line (the comment's L<line>), else 0
} BpDbgNode;
typedef struct BpDbgSite { uint32_t pc; uint16_t node; uint8_t ord; } BpDbgSite; // node: an index of dbg_nodes
typedef struct BpDbgVar { const struct BpAsset* owner; uint16_t var, slot; } BpDbgVar; // owner->vars[var] at slot

enum { BPD_CONTINUE = 1, BPD_STEP_OVER, BPD_STEP_INTO, BPD_STEP_OUT };

// --- Breakpoints (kept while the editor runs; not saved in the asset, like Unreal's per-user ones) ---
bool bp_debug_toggle(struct BpAsset* owner, uint32_t graph, uint32_t node_id); // true: it has one now
bool bp_debug_has(const struct BpAsset* owner, uint32_t graph, uint32_t node_id, bool* enabled);
void bp_debug_enable(struct BpAsset* owner, uint32_t graph, uint32_t node_id, bool on);
void bp_debug_clear(const struct BpAsset* owner); // NULL: every breakpoint
typedef struct BpDbgBreak { const struct BpAsset* owner; uint32_t node_id; uint16_t graph; bool enabled; uint32_t hits; } BpDbgBreak;
uint32_t bp_debug_breakpoints(BpDbgBreak* out, uint32_t cap);

// --- Paused ---
bool bp_debug_paused(void);       // stopped at a breakpoint or a step (the world waits)
void bp_debug_command(int cmd);   // BPD_*: done at the next bp_tick (the editor's frame)
// A frame of the call stack: the class running, its actor, the node (in owner's graph), what it is ("Event Tick",
// "Function AddTwice", "Custom Event Hit on BP_Enemy2"). The paused node first, the outer ones after
typedef struct BpDbgFrame {
    const struct BpAsset* cls;
    const struct BpAsset* owner;
    Entity actor;
    uint32_t node_id;
    uint16_t graph;
    int32_t line;
    char what[96];
} BpDbgFrame;
uint32_t bp_debug_stack(BpDbgFrame* out, uint32_t cap);
// The value of a pin of a node (owner's graph) in the actor's instance (ENTITY_NONE: the paused one, else the class's
// debug object) as text; false: no such instance or the pin holds nothing yet. An input shows the output it is wired to
bool bp_debug_pin_text(const struct BpAsset* owner, uint32_t graph, uint32_t node_id, int pin, Entity actor, char* out, size_t cap);
// The instance's variables (its class's and its parents'): name, type's label and value; false past the last
bool bp_debug_var_text(Entity actor, uint32_t i, char* name, size_t ncap, char* out, size_t cap);
Entity bp_debug_paused_actor(void);

// --- The instance to debug (Unreal's Debug Object): its breakpoints stop only there; ENTITY_NONE: any instance ---
void bp_debug_set_object(const struct BpAsset* cls, Entity e);
Entity bp_debug_object(const struct BpAsset* cls);
uint32_t bp_debug_instances(const struct BpAsset* cls, Entity* out, uint32_t cap); // the actors of the class playing

// --- The wires that ran: while a class is watched (its editor open in the Play) its nodes' starts are traced ---
void bp_debug_watch(const struct BpAsset* owner, bool on);
// Seconds since the node's input ord ran in the debug object's instance (or any, without one); < 0: not lately
float bp_debug_node_age(const struct BpAsset* owner, uint32_t graph, uint32_t node_id, int ord);

// --- Scripts (Real, C#, Python, C): their classes are graphs whose nodes carry their source line ---
uint32_t bp_debug_toggle_line(struct BpAsset* cls, int line); // the line's executable nodes; how many have one now
bool bp_debug_line_has(const struct BpAsset* cls, int line);
int bp_debug_paused_line(const struct BpAsset* cls);           // the paused node's line in this class's script, else 0

// --- For the tests and the Debug Viewer ---
typedef struct BpDbgStats { uint32_t patches, hits, stops, steps, outside; double resume_ms; } BpDbgStats;
const BpDbgStats* bp_debug_stats(void);
void bp_debug_compiled(struct BpAsset* a); // (bp_compile: the breakpoints put back in its new code)
void bp_debug_forget(struct BpAsset* a);   // (bp_compile, bp_free_asset: its patches dropped)
void bp_debug_moved(struct BpAsset* from, struct BpAsset* to); // (bp_adopt: from's contents now in to)
void bp_debug_end_play(void);              // (bp_end_play: a pause abandoned, the steps and traces gone)
struct Editor;
bool bp_debug_test_setup(struct Editor* ed); // --bp-debug-test (bp_debug_test.c): stop, inspect, step, continue, the cost
bool bp_debug_test_check(struct Editor* ed);
void bp_debug_test_frame(struct Editor* ed); // (every frame of the test, paused or not)
