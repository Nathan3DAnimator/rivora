// Memory without malloc in the frame: address space reserved once (VirtualAlloc) and committed as it fills, so
// arrays grow in place (pointers stay valid, nothing is copied), plus a linear allocator reset every frame.
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define RE_ALIGN 64 // every allocation starts on a cache line

// A growable array that never moves: `reserved` bytes of address space, the first `committed` of them usable.
typedef struct ReVmBuf {
    uint8_t* base;
    size_t reserved;
    size_t committed;
} ReVmBuf;

bool re_vm_reserve(ReVmBuf* b, size_t bytes);
// Commits up to at least `bytes` (rounded up to 64 KB). False when it doesn't fit the reservation or the OS refuses.
bool re_vm_ensure(ReVmBuf* b, size_t bytes);
void re_vm_release(ReVmBuf* b);

// Linear allocator: allocations are bumps of an offset; re_arena_reset frees all of them at once.
typedef struct ReArena {
    ReVmBuf vm;
    size_t used;
    size_t peak;
} ReArena;

bool re_arena_init(ReArena* a, size_t reserve_bytes);
void* re_arena_alloc(ReArena* a, size_t bytes); // RE_ALIGN aligned, uninitialized; NULL when the reservation is full
void re_arena_reset(ReArena* a);
void re_arena_free(ReArena* a);

// The frame arena: what lives only until the end of the frame (UI lists, temporary strings).
extern ReArena g_frame_arena;
#define RE_FRAME_ALLOC(type, n) ((type*)re_arena_alloc(&g_frame_arena, sizeof(type) * (size_t)(n)))
