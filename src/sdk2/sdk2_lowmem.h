#ifndef _MELEE_SDK2_LOWMEM_H_
#define _MELEE_SDK2_LOWMEM_H_

// Low-memory allocator. Two retail behaviors force the address layout:
//
//  1. Several host pointers travel through u32 (ARQPostRequest
//     source/dest, HSD_DevComRequest targets) — lossless only below 4 GB.
//  2. lbmemory tells MRAM apart from ARAM with `(u32)ptr < 0x80000000`
//     (retail MRAM lived at 0x80000000+).
//
// So the arena and all OS heaps are allocated at/near 0x80000000,
// mimicking the retail MRAM window [0x80000000, 0x100000000):
//   - Linux x86-64: mmap(MAP_FIXED_NOREPLACE, 0x80000000), then MAP_32BIT
//   - Windows: VirtualAlloc with 0x80000000-first hints
//   - other: malloc fallback (ARQ validates and panics with a clear
//     message instead of corrupting memory).
//
// The ARAM pool is small offsets (< 16 MB) exactly like hardware.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

void* sdk2_low_alloc(size_t size);
void sdk2_low_free(void* p, size_t size);
int sdk2_is_low(const void* p); // 1 if truncation to u32 is lossless

#ifdef __cplusplus
}
#endif

#endif
