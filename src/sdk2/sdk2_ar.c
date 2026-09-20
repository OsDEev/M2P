// SDK2 AR/ARQ: Audio RAM as a host pool.
//
// Retail ARAM is 16 MB at addresses 0..size. The game addresses it with
// small u32 values (ARAlloc results, HSD_SynthSFXBank entries), so the
// pool is a plain malloc'd block and "ARAM address" == byte offset.
// MRAM sides of transfers arrive as (possibly truncated) host pointers;
// they must point into the low arena (see sdk2_lowmem) and are validated
// — a miss panics with a clear message instead of corrupting memory.
//
// ARQ requests execute synchronously inside ARQPostRequest, then the
// callback fires. This matches retail ordering for the game's single-
// threaded state machines (devcom, lbarq).

#include "sdk2.h"
#include "sdk2_lowmem.h"

#include <dolphin/ar.h>
#include <dolphin/os.h>

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ARAM_SIZE (16u * 1024u * 1024u)

static u8* s_aram;
static u32 s_aram_top; // bump-up allocator cursor
static int s_aram_init;
static u32 s_chunk_size = 4096;
static ARQCallback s_dma_cb;

typedef struct {
    u32 addr;
    u32 size;
} AramAlloc;

#define ARAM_ALLOCS_MAX 64
static AramAlloc s_allocs[ARAM_ALLOCS_MAX];
static int s_alloc_count;

void* sdk2_ar_host(u32 aram_addr)
{
    if (s_aram && aram_addr < ARAM_SIZE)
        return s_aram + aram_addr;
    return NULL;
}

u32 sdk2_ar_size(void)
{
    return ARAM_SIZE;
}

static void* mram_ptr(u32 v, u32 len, const char* what)
{
    void* p = (void*) (uintptr_t) v;
    if (!sdk2_is_low(p)) {
        OSPanic("sdk2_ar.c", 0,
                "%s: MRAM pointer 0x%08X out of low-memory range "
                "(exe must map arena below 4 GB)",
                what, v);
    }
    (void) len;
    return p;
}

static int is_aram(u32 v, u32 len)
{
    return v + len <= ARAM_SIZE && v < ARAM_SIZE;
}

int ARCheckInit(void)
{
    return s_aram_init ? 1 : 0;
}

u32 ARInit(u32* stack_index_addr, u32 num_entries)
{
    (void) num_entries;
    if (!s_aram) {
        s_aram = (u8*) sdk2_low_alloc(ARAM_SIZE);
        if (!s_aram)
            s_aram = (u8*) malloc(ARAM_SIZE);
        if (!s_aram)
            OSPanic("sdk2_ar.c", 0, "ARAM pool alloc failed");
        memset(s_aram, 0, ARAM_SIZE);
    }
    s_aram_top = 0;
    s_alloc_count = 0;
    s_aram_init = 1;
    if (stack_index_addr)
        *stack_index_addr = num_entries;
    printf("[sdk2] ARAM: %u MB at %p\n", ARAM_SIZE / (1024u * 1024u),
           (void*) s_aram);
    return ARAM_SIZE;
}

void ARReset(void)
{
    s_aram_top = 0;
    s_alloc_count = 0;
}

void ARSetSize(void)
{
}

u32 ARGetBaseAddress(void)
{
    return 0;
}

u32 ARGetSize(void)
{
    return ARAM_SIZE;
}

u32 ARAlloc(u32 length)
{
    u32 addr;
    length = (length + 31) & ~31u;
    if (s_aram_top + length > ARAM_SIZE)
        return 0; // out of ARAM (retail returns 0 too)
    addr = s_aram_top;
    s_aram_top += length;
    if (s_alloc_count < ARAM_ALLOCS_MAX) {
        s_allocs[s_alloc_count].addr = addr;
        s_allocs[s_alloc_count].size = length;
        s_alloc_count++;
    }
    memset(s_aram + addr, 0, length);
    return addr;
}

u32 ARFree(u32* length)
{
    // Stack free of the most recent allocation (matches lbmemory's
    // alloc-then-free probe and is a safe superset for allocate-once
    // audio banks).
    if (s_alloc_count > 0) {
        s_alloc_count--;
        s_aram_top = s_allocs[s_alloc_count].addr;
        if (length)
            *length = ARAM_SIZE - s_aram_top;
        return s_allocs[s_alloc_count].addr;
    }
    if (length)
        *length = ARAM_SIZE - s_aram_top;
    return 0;
}

ARQCallback ARRegisterDMACallback(ARQCallback callback)
{
    ARQCallback prev = s_dma_cb;
    s_dma_cb = callback;
    return prev;
}

u32 ARGetDMAStatus(void)
{
    return 0; // idle (all transfers complete synchronously)
}

void ARStartDMA(u32 type, u32 mainmem_addr, u32 aram_addr, u32 length)
{
    void* mram = mram_ptr(mainmem_addr, length, "ARStartDMA");
    void* aram = sdk2_ar_host(aram_addr);
    if (!aram)
        OSPanic("sdk2_ar.c", 0, "ARStartDMA: bad ARAM addr 0x%08X",
                aram_addr);
    if (type == ARAM_DIR_MRAM_TO_ARAM)
        memcpy(aram, mram, length);
    else
        memcpy(mram, aram, length);
    if (s_dma_cb) {
        ARQRequest dummy;
        memset(&dummy, 0, sizeof(dummy));
        s_dma_cb(&dummy);
    }
}

// ------------------------------------------------------------ ARQ
void ARQInit(void)
{
}

void ARQReset(void)
{
}

void ARQPostRequest(struct ARQRequest* request, u32 owner, u32 type,
                    u32 priority, u32 source, u32 dest, u32 length,
                    ARQCallback callback)
{
    void* src;
    void* dst;
    if (!request)
        return;
    request->owner = owner;
    request->type = type;
    request->priority = priority;
    request->source = source;
    request->dest = dest;
    request->length = length;
    request->callback = callback;
    request->next = NULL;
    if (type == ARQ_TYPE_MRAM_TO_ARAM) {
        // source = MRAM host pointer, dest = ARAM offset
        src = mram_ptr(source, length, "ARQPostRequest/src");
        dst = sdk2_ar_host(dest);
        if (!dst) {
            OSPanic("sdk2_ar.c", 0,
                    "ARQPostRequest: bad ARAM dest 0x%08X", dest);
        }
    } else {
        // source = ARAM offset, dest = MRAM host pointer
        src = sdk2_ar_host(source);
        if (!src && !(length == 0)) {
            // tolerate zero-length no-ops with odd addresses
            OSPanic("sdk2_ar.c", 0,
                    "ARQPostRequest: bad ARAM src 0x%08X", source);
        }
        dst = mram_ptr(dest, length, "ARQPostRequest/dst");
    }
    if (length && src && dst) {
        // clamp to pool on the ARAM side (retail DMA faults; we clamp)
        memcpy(dst, src, length);
    }
    if (callback)
        callback(request);
}

void ARQRemoveRequest(struct ARQRequest* request)
{
    (void) request; // already executed
}

void ARQRemoveOwnerRequest(u32 owner)
{
    (void) owner;
}

void ARQFlushQueue(void)
{
}

void ARQSetChunkSize(u32 size)
{
    s_chunk_size = size ? size : 4096;
}

u32 ARQGetChunkSize(void)
{
    return s_chunk_size;
}
