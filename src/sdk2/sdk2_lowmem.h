#ifndef _MELEE_SDK2_LOWMEM_H_
#define _MELEE_SDK2_LOWMEM_H_

// Low-memory allocator.
//
// Two retail behaviors force the address layout:
//
//  1. The port targets 32-bit x86 (struct layouts must match the
//     GameCube: 4-byte pointers). All pointers therefore fit in u32 and
//     survive the game's u32 pointer flows losslessly.
//  2. lbmemory/lbfile/ftdata tell MRAM apart from ARAM by address.
//     ARAM is always below 16 MB, so the MRAM arena is allocated at or
//     above LOWMEM_FLOOR (32 MB) and classification is
//     `addr < 16 MB -> ARAM, else MRAM` (see sdk2_addr_is_aram()).
//
// sdk2_low_alloc() honors the floor and returns NULL (rather than a
// low alias) when it cannot, so callers fail fast with a clear message
// instead of corrupting memory. The ARAM pool itself may live anywhere;
// only the MRAM arena needs the floor.

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define LOWMEM_FLOOR 0x02000000u // 32 MB

void* sdk2_low_alloc(size_t size);
void sdk2_low_free(void* p, size_t size);
int sdk2_is_low(const void* p); // 1 if truncation to u32 is lossless

#ifdef __cplusplus
}
#endif

#endif
