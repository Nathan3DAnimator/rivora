#include "mem.h"

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <sys/mman.h>
#endif

#define RE_COMMIT_STEP ((size_t)64 * 1024)

ReArena g_frame_arena;

bool re_vm_reserve(ReVmBuf* b, size_t bytes) {
    bytes = (bytes + RE_COMMIT_STEP - 1) & ~(RE_COMMIT_STEP - 1);
#ifdef _WIN32
    b->base = VirtualAlloc(NULL, bytes, MEM_RESERVE, PAGE_NOACCESS);
#else
    void* p = mmap(NULL, bytes, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    b->base = p == MAP_FAILED ? NULL : p;
#endif
    b->reserved = b->base ? bytes : 0;
    b->committed = 0;
    return b->base != NULL;
}

bool re_vm_ensure(ReVmBuf* b, size_t bytes) {
    if (bytes <= b->committed) return true;
    if (bytes > b->reserved) return false;
    size_t want = (bytes + RE_COMMIT_STEP - 1) & ~(RE_COMMIT_STEP - 1);
    if (want > b->reserved) want = b->reserved;
#ifdef _WIN32
    if (!VirtualAlloc(b->base + b->committed, want - b->committed, MEM_COMMIT, PAGE_READWRITE)) return false;
#else
    if (mprotect(b->base + b->committed, want - b->committed, PROT_READ | PROT_WRITE) != 0) return false;
#endif
    b->committed = want; // fresh pages read as zero
    return true;
}

void re_vm_release(ReVmBuf* b) {
#ifdef _WIN32
    if (b->base) VirtualFree(b->base, 0, MEM_RELEASE);
#else
    if (b->base) munmap(b->base, b->reserved);
#endif
    b->base = NULL;
    b->reserved = b->committed = 0;
}

bool re_arena_init(ReArena* a, size_t reserve_bytes) {
    a->used = a->peak = 0;
    return re_vm_reserve(&a->vm, reserve_bytes);
}

void* re_arena_alloc(ReArena* a, size_t bytes) {
    size_t start = (a->used + RE_ALIGN - 1) & ~(size_t)(RE_ALIGN - 1);
    if (!re_vm_ensure(&a->vm, start + bytes)) return NULL;
    a->used = start + bytes;
    if (a->used > a->peak) a->peak = a->used;
    return a->vm.base + start;
}

void re_arena_reset(ReArena* a) { a->used = 0; } // the pages stay committed: the next frame reuses them

void re_arena_free(ReArena* a) {
    re_vm_release(&a->vm);
    a->used = a->peak = 0;
}
