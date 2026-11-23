// The Blueprint debugger's fiber (bp_debug.h): while breakpoints are set, the Play's ticks run on a stack of their own
// so a breakpoint can leave them half done (the VM's frames kept on that stack) and the editor go on drawing frames.
// Windows fibers: switching is a few hundred ns, twice a tick, only while debugging. Elsewhere (Android: the packaged
// game, no debugger) the work just runs.
#include "bp_fiber.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

static void* g_main;   // the thread as a fiber
static void* g_vm;     // the VM's fiber (made once, again after an abandon)
static bool g_in_vm, g_done = true;
static void (*g_fn)(void*);
static void* g_arg;

static void WINAPI vm_proc(void* unused) {
    (void)unused;
    for (;;) { // (a fiber never returns: it would end the thread)
        g_fn(g_arg);
        g_done = true;
        g_in_vm = false;
        SwitchToFiber(g_main);
    }
}

static bool ready(void) {
    if (!g_main) g_main = IsThreadAFiber() ? GetCurrentFiber() : ConvertThreadToFiber(NULL);
    if (g_main && !g_vm) g_vm = CreateFiberEx(64 * 1024, 16u << 20, FIBER_FLAG_FLOAT_SWITCH, vm_proc, NULL); // (16 MB reserved, committed as used)
    return g_main && g_vm;
}

bool bpf_run(void (*fn)(void*), void* arg) {
    if (g_in_vm || !g_done || !ready()) { // (already inside, or a job waits: the caller runs it here)
        fn(arg);
        return true;
    }
    g_fn = fn, g_arg = arg, g_done = false, g_in_vm = true;
    SwitchToFiber(g_vm);
    g_in_vm = false;
    return g_done;
}

void bpf_suspend(void) {
    if (!g_in_vm) return;
    g_in_vm = false;
    SwitchToFiber(g_main);
    g_in_vm = true;
}

bool bpf_resume(void) {
    if (g_done || g_in_vm || !g_vm) return true;
    g_in_vm = true;
    SwitchToFiber(g_vm);
    g_in_vm = false;
    return g_done;
}

bool bpf_in_vm(void) { return g_in_vm; }
bool bpf_waiting(void) { return !g_done; }

void bpf_abandon(void) {
    if (g_done || g_in_vm) return;
    DeleteFiber(g_vm); // (its stack and the half-done work with it)
    g_vm = NULL;
    g_done = true;
}
#else
bool bpf_run(void (*fn)(void*), void* arg) {
    fn(arg);
    return true;
}
void bpf_suspend(void) {}
bool bpf_resume(void) { return true; }
bool bpf_in_vm(void) { return false; }
bool bpf_waiting(void) { return false; }
void bpf_abandon(void) {}
#endif
