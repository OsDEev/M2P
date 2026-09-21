#include "sdk2_lowmem.h"

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <stdint.h>
#include <stdlib.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#elif defined(__linux__)
#include <sys/mman.h>
#ifndef MAP_32BIT
#define MAP_32BIT 0x40
#endif
#include <unistd.h>

static void* s_mapped[64];

int sdk2_low_is_mapped(const void* p)
{
    int i;
    for (i = 0; i < 64; i++) {
        if (s_mapped[i] == p)
            return 1;
    }
    return 0;
}

static void low_track(void* p)
{
    int i;
    for (i = 0; i < 64; i++) {
        if (!s_mapped[i]) {
            s_mapped[i] = p;
            return;
        }
    }
}

static void low_untrack(void* p)
{
    int i;
    for (i = 0; i < 64; i++) {
        if (s_mapped[i] == p)
            s_mapped[i] = NULL;
    }
}
#endif

// Regions below LOWMEM_FLOOR are refused: MRAM must never alias the
// ARAM window (< 16 MB), or the addr<16MB -> ARAM classification used
// by lbmemory/lbfile/ftdata breaks. NULL means "fail fast at the call
// site", never a silent low alias.
static int below_floor(const void* p)
{
    return p != NULL && (uintptr_t) p < (uintptr_t) LOWMEM_FLOOR;
}

void* sdk2_low_alloc(size_t size)
{
    void* p = NULL;
    if (size == 0)
        size = 4096;
    // round up to 64 KiB (allocation granularity on both platforms)
    size = (size + 0xFFFFu) & ~(size_t) 0xFFFFu;
#ifdef _WIN32
    {
        // 0x80000000 first: mimics the retail MRAM window.
        static uintptr_t hints[] = { 0x80000000u, 0x88000000u,
                                     0x90000000u, 0xA0000000u,
                                     0x20000000u, 0x30000000u };
        unsigned i;
        for (i = 0; i < sizeof(hints) / sizeof(hints[0]); i++) {
            p = VirtualAlloc((LPVOID) hints[i], size,
                             MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
            if (p != NULL) {
                if (below_floor(p)) {
                    VirtualFree(p, 0, MEM_RELEASE);
                    continue;
                }
                return p;
            }
        }
        // Last resort: any address, still honoring the floor.
        p = VirtualAlloc(NULL, size, MEM_RESERVE | MEM_COMMIT,
                         PAGE_READWRITE);
        if (below_floor(p)) {
            VirtualFree(p, 0, MEM_RELEASE);
            return NULL;
        }
        return p;
    }
#elif defined(__linux__) && defined(__x86_64__)
#ifdef MAP_FIXED_NOREPLACE
    p = mmap((void*) 0x80000000u, size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (p != MAP_FAILED) {
        low_track(p);
        return p; // fixed hint is always above the floor
    }
    p = NULL;
#endif
    p = mmap(NULL, size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
    if (p == MAP_FAILED)
        p = NULL;
    if (p != NULL) {
        if (below_floor(p)) {
            munmap(p, size);
            return NULL;
        }
        low_track(p);
        return p;
    }
    p = malloc(size);
    if (below_floor(p)) {
        free(p);
        return NULL;
    }
    return p;
#else
    p = malloc(size);
    if (below_floor(p)) {
        free(p);
        return NULL;
    }
    return p;
#endif
}

void sdk2_low_free(void* p, size_t size)
{
    if (!p)
        return;
#ifdef _WIN32
    {
        MEMORY_BASIC_INFORMATION mi;
        if (VirtualQuery(p, &mi, sizeof(mi)) &&
            mi.Type == MEM_PRIVATE && mi.AllocationBase == p) {
            VirtualFree(p, 0, MEM_RELEASE);
            return;
        }
        free(p);
    }
#elif defined(__linux__) && defined(__x86_64__)
    if (p && sdk2_low_is_mapped(p)) {
        low_untrack(p);
        munmap(p, size);
        return;
    }
    (void) size;
#else
    (void) size;
    free(p);
#endif
}

int sdk2_is_low(const void* p)
{
    return (uintptr_t) p < 0x100000000ull;
}
