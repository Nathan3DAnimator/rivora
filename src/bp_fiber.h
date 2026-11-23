// The Blueprint debugger's fiber (bp_fiber.c, bp_debug.h): work that a breakpoint can leave half done
#pragma once
#include <stdbool.h>

// Runs fn(arg) on the VM's fiber; true when it finished, false when it was suspended (bpf_resume goes on with it).
// Inside the fiber already, or with work waiting: run here directly (true)
bool bpf_run(void (*fn)(void*), void* arg);
void bpf_suspend(void);  // (from the fiber) back to bpf_run's / bpf_resume's caller; returns when resumed
bool bpf_resume(void);   // the waiting work goes on; true when it finished
bool bpf_in_vm(void);    // running on the VM's fiber now
bool bpf_waiting(void);  // work suspended, waiting for bpf_resume
void bpf_abandon(void);  // the waiting work dropped (the Play stopped while paused)
