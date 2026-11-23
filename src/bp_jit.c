// The Blueprints' (and the scripts') native code: an x64 JIT of the register bytecode (notas-dev/scripts.md, "Compilação
// nativa"). Mixed mode: every op of a class gets a place in one machine-code function. The ops that only move numbers
// (moves, jumps, the arithmetic, the comparisons, the vector math) are compiled; the others are a stub that returns
// their pc, and the interpreter runs that one op and calls back in (bp_vm.c run). Leaf code: no calls, no stack, only
// volatile registers (rcx: the instance's registers, rdx: the constants, r8: the context, xmm0-xmm3), so it needs no
// unwind data. A loop's back jump takes its ops from the budget (an infinite loop still stops).
#if defined(_WIN32) && (defined(_M_X64) || defined(__x86_64__))
#define RE_JIT_X64 1
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "blueprint.h"
#include "bp_internal.h"
#include "bp_jit.h"

bool g_bp_jit = true; // REALENGINE_JIT=0: the interpreter only (the measures, a suspected JIT bug)
BpJitStats g_bp_jit_stats;

typedef struct Patch { uint32_t at, pc; } Patch; // a rel32 to fill with the label of pc
typedef struct Em {
    uint8_t* p;
    uint32_t n, cap;
    uint32_t* label; // per pc: its code's offset
    Patch* patch;
    uint32_t npatch, cap_patch;
    uint32_t consts; // the constants' offset (16-aligned)
    bool fail;
} Em;

static void b1(Em* e, uint8_t x) {
    if (e->n >= e->cap) {
        uint32_t cap = e->cap ? e->cap * 2 : 4096;
        uint8_t* p = realloc(e->p, cap);
        if (!p) {
            e->fail = true;
            return;
        }
        e->p = p, e->cap = cap;
    }
    e->p[e->n++] = x;
}
static void b4(Em* e, uint32_t x) {
    for (int i = 0; i < 4; i++) b1(e, (uint8_t)(x >> (8 * i)));
}
// [rcx + slot*16] (a register) or [rdx + slot*16] (a constant): ModRM mod 10, disp32
static void mem(Em* e, int reg, uint16_t x) {
    b1(e, (uint8_t)(0x80 | (reg << 3) | ((x & BP_K) ? 2 : 1)));
    b4(e, (uint32_t)(x & 0x7FFFu) * 16u);
}
// SSE: [prefix] 0F op, a register / memory operand
static void sse_m(Em* e, uint8_t prefix, uint8_t op, int xmm, uint16_t x) {
    if (prefix) b1(e, prefix);
    b1(e, 0x0F), b1(e, op);
    mem(e, xmm, x);
}
static void sse_r(Em* e, uint8_t prefix, uint8_t op, int dst, int src) {
    if (prefix) b1(e, prefix);
    b1(e, 0x0F), b1(e, op), b1(e, (uint8_t)(0xC0 | (dst << 3) | src));
}
// [rip + the constant c] (tail: bytes after the disp32, an imm8)
enum { K_ONE = 0, K_ABS = 16, K_EPS = 32, K_XYZ = 48 };
static void sse_k(Em* e, uint8_t prefix, uint8_t op, int xmm, int c, int tail) {
    if (prefix) b1(e, prefix);
    b1(e, 0x0F), b1(e, op), b1(e, (uint8_t)(0x05 | (xmm << 3)));
    b4(e, (uint32_t)((int32_t)(e->consts + (uint32_t)c) - (int32_t)(e->n + 4 + (uint32_t)tail)));
}
static void movss_ld(Em* e, int xmm, uint16_t x) { sse_m(e, 0xF3, 0x10, xmm, x); } // (the upper lanes zeroed)
static void movups_ld(Em* e, int xmm, uint16_t x) { sse_m(e, 0, 0x10, xmm, x); }
static void movups_st(Em* e, uint16_t x, int xmm) { sse_m(e, 0, 0x11, xmm, x); }
// movss [rcx + slot*16 + off], xmm (one lane of a register slot)
static void movss_st(Em* e, uint16_t x, uint32_t off, int xmm) {
    b1(e, 0xF3), b1(e, 0x0F), b1(e, 0x11), b1(e, (uint8_t)(0x80 | (xmm << 3) | 1));
    b4(e, (uint32_t)(x & 0x7FFFu) * 16u + off);
}
static void cmpss_r(Em* e, int dst, int src, uint8_t pred) { sse_r(e, 0xF3, 0xC2, dst, src), b1(e, pred); }
static void cmpss_m(Em* e, int dst, uint16_t x, uint8_t pred) { sse_m(e, 0xF3, 0xC2, dst, x), b1(e, pred); }
static void roundss(Em* e, int xmm, uint8_t mode) { b1(e, 0x66), b1(e, 0x0F), b1(e, 0x3A), b1(e, 0x0A), b1(e, (uint8_t)(0xC0 | (xmm << 3) | xmm)), b1(e, mode); }
static void xorps(Em* e, int a, int b) { sse_r(e, 0, 0x57, a, b); }
enum { P_EQ = 0, P_LT = 1, P_LE = 2, P_NEQ = 4, P_NLT = 5, P_NLE = 6 }; // cmpss predicates

static void jump_to(Em* e, uint8_t cc, uint32_t pc) { // cc 0: jmp; else 0F cc
    if (cc) b1(e, 0x0F), b1(e, cc);
    else b1(e, 0xE9);
    if (e->npatch == e->cap_patch) {
        uint32_t cap = e->cap_patch ? e->cap_patch * 2 : 64;
        Patch* p = realloc(e->patch, sizeof *p * cap);
        if (!p) {
            e->fail = true;
            return;
        }
        e->patch = p, e->cap_patch = cap;
    }
    e->patch[e->npatch++] = (Patch){ e->n, pc };
    b4(e, 0);
}
static void exit_with(Em* e, uint32_t pc) { b1(e, 0xB8), b4(e, pc), b1(e, 0xC3); } // mov eax, pc; ret
#define PC_BUDGET 0xFFFFFFFEu // (a pseudo pc: the budget's exit)
// a jump back: the loop's ops off the budget (sub dword [r8], n; jle the budget's exit)
static void budget(Em* e, uint32_t n) {
    b1(e, 0x41), b1(e, 0x81), b1(e, 0x28), b4(e, n);
    jump_to(e, 0x8E, PC_BUDGET);
}
// a conditional jump to t: the opposite condition skips over the (budget and the) jump
static void branch(Em* e, uint8_t skip_cc8, uint32_t pc, uint32_t t) {
    b1(e, skip_cc8);
    uint32_t at = e->n;
    b1(e, 0);
    if (t <= pc) budget(e, pc - t + 1);
    jump_to(e, 0, t);
    e->p[at] = (uint8_t)(e->n - at - 1);
}
// the scalar result in xmm0 (its upper lanes zero) into r[d]
static void store(Em* e, uint16_t d) { movups_st(e, d, 0); }

// The native ops it compiles (the rest: the interpreter's, bp_vm.c native)
static bool native_op(Em* e, uint32_t kind, const BpOp* o) {
    uint16_t A = o->a, B = o->b, C = o->c, D = o->d;
    switch (kind) {
    case BPK_ADD: case BPK_IADD: case BPK_SUB: case BPK_ISUB: case BPK_MUL: case BPK_IMUL:
    case BPK_FMAX: case BPK_IMAX: case BPK_FMIN: case BPK_IMIN: {
        uint8_t op = kind == BPK_ADD || kind == BPK_IADD ? 0x58 : kind == BPK_SUB || kind == BPK_ISUB ? 0x5C : kind == BPK_MUL || kind == BPK_IMUL ? 0x59
                   : kind == BPK_FMAX || kind == BPK_IMAX ? 0x5F : 0x5D;
        movss_ld(e, 0, A);
        sse_m(e, 0xF3, op, 0, B);
        store(e, D);
        return true;
    }
    case BPK_DIV: // B != 0 ? A / B : 0
        movss_ld(e, 0, A), sse_m(e, 0xF3, 0x5E, 0, B);
        movss_ld(e, 1, B), xorps(e, 2, 2), cmpss_r(e, 1, 2, P_NEQ);
        sse_r(e, 0, 0x54, 0, 1); // andps: 0 where B was 0 (an inf / nan dropped)
        store(e, D);
        return true;
    case BPK_GREATER: case BPK_IGREATER: case BPK_LESS: case BPK_ILESS: case BPK_IGE: case BPK_FGE: case BPK_ILE: case BPK_FLE:
    case BPK_IEQUAL: case BPK_INOTEQUAL: case BPK_FNOTEQUAL: {
        uint8_t pr = kind == BPK_GREATER || kind == BPK_IGREATER ? P_NLE : kind == BPK_LESS || kind == BPK_ILESS ? P_LT
                   : kind == BPK_IGE || kind == BPK_FGE ? P_NLT : kind == BPK_ILE || kind == BPK_FLE ? P_LE : kind == BPK_IEQUAL ? P_EQ : P_NEQ;
        movss_ld(e, 0, A), cmpss_m(e, 0, B, pr);
        sse_k(e, 0, 0x54, 0, K_ONE, 0); // the mask -> 1.0 / 0.0
        store(e, D);
        return true;
    }
    case BPK_EQUAL: // |A - B| < 1e-4
        movss_ld(e, 0, A), sse_m(e, 0xF3, 0x5C, 0, B);
        sse_k(e, 0, 0x54, 0, K_ABS, 0);
        sse_k(e, 0xF3, 0xC2, 0, K_EPS, 1), b1(e, P_LT);
        sse_k(e, 0, 0x54, 0, K_ONE, 0);
        store(e, D);
        return true;
    case BPK_AND: case BPK_OR: case BPK_NOT:
        xorps(e, 2, 2);
        movss_ld(e, 0, A), cmpss_r(e, 0, 2, kind == BPK_NOT ? P_EQ : P_NEQ);
        if (kind != BPK_NOT) {
            movss_ld(e, 1, B), cmpss_r(e, 1, 2, P_NEQ);
            sse_r(e, 0, kind == BPK_AND ? 0x54 : 0x56, 0, 1); // andps / orps
        }
        sse_k(e, 0, 0x54, 0, K_ONE, 0);
        store(e, D);
        return true;
    case BPK_SELECT: // A != 0 ? B : C
        xorps(e, 2, 2);
        movss_ld(e, 0, A), cmpss_r(e, 0, 2, P_NEQ);
        movss_ld(e, 1, B), movss_ld(e, 3, C);
        sse_r(e, 0, 0x54, 1, 0); // B & m
        sse_r(e, 0, 0x55, 0, 3); // ~m & C
        sse_r(e, 0, 0x56, 0, 1);
        store(e, D);
        return true;
    case BPK_INT_TO_FLOAT: movss_ld(e, 0, A), store(e, D); return true;
    case BPK_FLOAT_TO_INT: case BPK_FLOOR: case BPK_CEIL:
        movss_ld(e, 0, A), roundss(e, 0, kind == BPK_FLOAT_TO_INT ? 3 : kind == BPK_FLOOR ? 1 : 2);
        store(e, D);
        return true;
    case BPK_FABS: case BPK_IABS: movss_ld(e, 0, A), sse_k(e, 0, 0x54, 0, K_ABS, 0), store(e, D); return true;
    case BPK_CLAMP: case BPK_ICLAMP: // min(max(A, B), C)
        movss_ld(e, 0, A), sse_m(e, 0xF3, 0x5F, 0, B), sse_m(e, 0xF3, 0x5D, 0, C);
        store(e, D);
        return true;
    case BPK_LERP: // A + (B - A) * C
        movss_ld(e, 0, B), sse_m(e, 0xF3, 0x5C, 0, A), sse_m(e, 0xF3, 0x59, 0, C), sse_m(e, 0xF3, 0x58, 0, A);
        store(e, D);
        return true;
    case BPK_SQRT: // A > 0 ? sqrt(A) : 0
        movss_ld(e, 1, A), sse_r(e, 0xF3, 0x51, 0, 1);
        xorps(e, 2, 2), cmpss_r(e, 1, 2, P_NLE);
        sse_r(e, 0, 0x54, 0, 1);
        store(e, D);
        return true;
    case BPK_VADD: case BPK_VSUB:
        movups_ld(e, 0, A), sse_m(e, 0, kind == BPK_VADD ? 0x58 : 0x5C, 0, B);
        sse_k(e, 0, 0x54, 0, K_XYZ, 0); // (w: 0, as the interpreter leaves it)
        store(e, D);
        return true;
    case BPK_VMULS:
        movups_ld(e, 0, A), movss_ld(e, 1, B);
        sse_r(e, 0, 0xC6, 1, 1), b1(e, 0); // shufps: B in every lane
        sse_r(e, 0, 0x59, 0, 1);
        sse_k(e, 0, 0x54, 0, K_XYZ, 0);
        store(e, D);
        return true;
    case BPK_MAKE_VECTOR: case BPK_MAKE_ROTATOR: // { A, B, C, 0 }
        movss_ld(e, 0, A), movss_ld(e, 1, B), movss_ld(e, 2, C);
        sse_r(e, 0, 0x14, 0, 1); // unpcklps: a b 0 0
        sse_r(e, 0, 0x16, 0, 2); // movlhps: a b c 0
        store(e, D);
        return true;
    case BPK_DELTA: // the frame's seconds (the context's)
        b1(e, 0xF3), b1(e, 0x41), b1(e, 0x0F), b1(e, 0x10), b1(e, 0x40), b1(e, 0x04); // movss xmm0, [r8 + 4]
        store(e, D);
        return true;
    default: return false;
    }
}

// One op at pc: compiled (true), or not (its stub returns its pc to the interpreter)
static bool op_native(Em* e, const BpAsset* a, uint32_t pc) {
    const BpOp* o = &a->code[pc];
    uint32_t n = a->ncode;
    switch (o->op) {
    case OP_MOVN:
        if (o->c < 1 || o->c > 4) return false;
        for (int i = 0; i < o->c; i++) movups_ld(e, i, (uint16_t)(o->b + i)); // (all read first: an overlap moves right)
        for (int i = 0; i < o->c; i++) movups_st(e, (uint16_t)(o->a + i), i);
        return true;
    case OP_JMP:
        if (o->a <= pc) budget(e, pc - o->a + 1);
        jump_to(e, 0, o->a < n ? o->a : n);
        return true;
    case OP_JZ: case OP_JNZ: { // a's x == 0 (a NaN isn't)
        uint32_t t = o->b < n ? o->b : n;
        movss_ld(e, 0, o->a), xorps(e, 1, 1);
        sse_r(e, 0, 0x2E, 0, 1); // ucomiss
        if (o->op == OP_JZ) { // jump when ZF and not PF
            b1(e, 0x7A), b1(e, 0); // jp skip (patched)
            uint32_t at = e->n - 1;
            branch(e, 0x75, pc, t); // jne skip
            e->p[at] = (uint8_t)(e->n - at - 1);
        } else { // jump when !ZF or PF
            uint32_t back = t;
            b1(e, 0x7A), b1(e, 2), b1(e, 0x74), b1(e, 0); // jp +2 (to the jump); je skip
            uint32_t at = e->n - 1;
            if (back <= pc) budget(e, pc - back + 1);
            jump_to(e, 0, back);
            e->p[at] = (uint8_t)(e->n - at - 1);
            // (the jp lands on the budget / jump: its offset is 2 past the je)
        }
        return true;
    }
    case OP_JGT: case OP_JGE: case OP_JEQ: { // a ? b, to c
        uint32_t t = o->c < n ? o->c : n;
        movss_ld(e, 0, o->a), movss_ld(e, 1, o->b);
        sse_r(e, 0, 0x2E, 0, 1);
        if (o->op == OP_JGT) branch(e, 0x76, pc, t);      // jbe skip (a > b: CF 0 and ZF 0)
        else if (o->op == OP_JGE) branch(e, 0x72, pc, t); // jb skip
        else {
            b1(e, 0x7A), b1(e, 0);
            uint32_t at = e->n - 1;
            branch(e, 0x75, pc, t);
            e->p[at] = (uint8_t)(e->n - at - 1);
        }
        return true;
    }
    case OP_INC: // r[a].x += 1 (its other lanes kept)
        movss_ld(e, 0, o->a), sse_k(e, 0xF3, 0x58, 0, K_ONE, 0), movss_st(e, o->a, 0, 0);
        return true;
    case OP_DELTA: b1(e, 0xF3), b1(e, 0x41), b1(e, 0x0F), b1(e, 0x10), b1(e, 0x40), b1(e, 0x04), store(e, o->a); return true;
    case OP_COMP: // r[a] <- { b's lane c, 0, 0, 0 }
        movups_ld(e, 0, o->b);
        if (o->c & 3) sse_r(e, 0, 0xC6, 0, 0), b1(e, (uint8_t)((o->c & 3) * 0x55)); // shufps: lane c everywhere
        xorps(e, 1, 1);
        sse_r(e, 0xF3, 0x10, 1, 0); // movss xmm1, xmm0: lane 0 only
        movups_st(e, o->a, 1);
        return true;
    case OP_SETCOMP: // r[a]'s lane c <- b's x
        movss_ld(e, 0, o->b), movss_st(e, o->a, (uint32_t)(o->c & 3) * 4u, 0);
        return true;
    default:
        if (o->op >= OP_NATIVE) return native_op(e, o->op - OP_NATIVE, o);
        return false;
    }
}

#ifdef RE_JIT_X64
void bp_jit_free(BpAsset* a) {
    if (a && a->jit) VirtualFree(a->jit, 0, MEM_RELEASE);
    if (a) a->jit = NULL, a->jit_ok = false;
}

bool bp_jit_build(BpAsset* a) {
    static int env = -1;
    if (env < 0) {
        const char* s = getenv("REALENGINE_JIT");
        env = s ? atoi(s) : 1;
        if (!env) g_bp_jit = false;
    }
    if (!g_bp_jit || !a || !a->compiled || !a->ncode || a->jit || a->jit_failed) return a && a->jit;
    double t0 = (double)GetTickCount64();
    LARGE_INTEGER f, c0, c1;
    QueryPerformanceFrequency(&f), QueryPerformanceCounter(&c0);
    (void)t0;
    uint32_t n = a->ncode;
    Em e = { 0 };
    e.label = calloc(n + 2, sizeof *e.label);
    if (!e.label) return false;
    // the constants (16-aligned at the start): 1.0; |x|; 1e-4; x y z
    static const uint32_t consts[16] = { 0x3F800000u, 0, 0, 0, 0x7FFFFFFFu, 0, 0, 0, 0x38D1B717u, 0, 0, 0, ~0u, ~0u, ~0u, 0 };
    e.consts = 0;
    for (int i = 0; i < 16; i++) b4(&e, consts[i]);
    uint32_t table = e.n; // the entry table: the code's address of each pc (filled at the end)
    for (uint32_t i = 0; i <= n; i++) b4(&e, 0), b4(&e, 0);
    // the entry: rax <- table[pc]; jmp rax  (r9d: the pc, zero extended)
    uint32_t entry = e.n;
    b1(&e, 0x45), b1(&e, 0x89), b1(&e, 0xC9);                                    // mov r9d, r9d
    b1(&e, 0x48), b1(&e, 0x8D), b1(&e, 0x05), b4(&e, (uint32_t)((int32_t)table - (int32_t)(e.n + 4))); // lea rax, [rip + table]
    b1(&e, 0x42), b1(&e, 0xFF), b1(&e, 0x24), b1(&e, 0xC8);                      // jmp [rax + r9*8]
    uint32_t compiled = 0;
    for (uint32_t pc = 0; pc < n && !e.fail; pc++) {
        e.label[pc] = e.n;
        uint32_t before = e.n, np = e.npatch;
        if (op_native(&e, a, pc)) compiled++;
        else e.n = before, e.npatch = np, exit_with(&e, pc);
    }
    e.label[n] = e.n, exit_with(&e, n); // (past the end: done)
    uint32_t budget_exit = e.n;
    exit_with(&e, PC_BUDGET);
    if (e.fail || compiled == 0) { // (nothing to gain: the interpreter alone)
        free(e.p), free(e.label), free(e.patch);
        a->jit_failed = true;
        return false;
    }
    for (uint32_t i = 0; i < e.npatch; i++) {
        uint32_t to = e.patch[i].pc == PC_BUDGET ? budget_exit : e.label[e.patch[i].pc <= n ? e.patch[i].pc : n];
        int32_t rel = (int32_t)to - (int32_t)(e.patch[i].at + 4);
        memcpy(e.p + e.patch[i].at, &rel, 4);
    }
    uint8_t* mem_ = VirtualAlloc(NULL, e.n, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    if (!mem_) {
        free(e.p), free(e.label), free(e.patch);
        a->jit_failed = true;
        return false;
    }
    memcpy(mem_, e.p, e.n);
    for (uint32_t i = 0; i <= n; i++) {
        uint64_t addr = (uint64_t)(uintptr_t)(mem_ + e.label[i]);
        memcpy(mem_ + table + i * 8, &addr, 8);
    }
    DWORD old;
    VirtualProtect(mem_, e.n, PAGE_EXECUTE_READ, &old);
    FlushInstructionCache(GetCurrentProcess(), mem_, e.n);
    a->jit = mem_;
    a->jit_entry = (uint32_t(*)(BpValue*, const BpValue*, void*, uint32_t))(void*)(mem_ + entry);
    a->jit_ok = true;
    QueryPerformanceCounter(&c1);
    g_bp_jit_stats.classes++, g_bp_jit_stats.ops += n, g_bp_jit_stats.native_ops += compiled, g_bp_jit_stats.bytes += e.n;
    g_bp_jit_stats.build_ms += (double)(c1.QuadPart - c0.QuadPart) * 1000.0 / (double)f.QuadPart;
    free(e.p), free(e.label), free(e.patch);
    return true;
}
#else // (ARM, Android: no native code; the bytecode is interpreted)
void bp_jit_free(BpAsset* a) { (void)a; }
bool bp_jit_build(BpAsset* a) {
    (void)a;
    return false;
}
#endif
