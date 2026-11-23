// The bytecode's native code (bp_jit.c): an x64 JIT in mixed mode with the interpreter (bp_vm.c run)
#pragma once
#include <stdbool.h>
#include <stdint.h>

struct BpAsset;
struct BpValue;

// What the code reads beside the registers: the budget (a loop's back jump takes its ops; <= 0: stopped) and the frame's
// seconds. Its layout is the code's (budget at 0, dt at 4)
typedef struct BpJitCtx { int32_t budget; float dt; } BpJitCtx;
// Runs from pc until an op it doesn't compile: returns that op's pc (the interpreter runs it); the code's length: done;
// BP_JIT_BUDGET: the budget ran out (an infinite loop)
#define BP_JIT_BUDGET 0xFFFFFFFEu

extern bool g_bp_jit; // on by default; REALENGINE_JIT=0 (and the measure's "off" half) turns it off
typedef struct BpJitStats { uint32_t classes, ops, native_ops, bytes; double build_ms; } BpJitStats;
extern BpJitStats g_bp_jit_stats;

bool bp_jit_build(struct BpAsset* a); // after its compile (bp_vm.c run: the first time it runs); false: interpreted
void bp_jit_free(struct BpAsset* a);  // its code dropped (a compile, the asset freed or replaced)
