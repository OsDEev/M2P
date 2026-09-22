// SDK2 OS: time, interrupts, arena/heap, alarms, threads, sync primitives,
// reset/reboot, RTC/calendar, cache stubs, context stubs.
//
// Threading model: game threads (OSThread) run on real host threads
// (Win32 / pthreads). The main thread is registered as an OSThread on
// first OS call. Alarm handlers fire on a dedicated watchdog thread.

#include "sdk2.h"
#include "sdk2_lowmem.h"

#include <dolphin/os.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <pthread.h>
#include <unistd.h>
#endif

// ------------------------------------------------------------ globals
u32 __OSBusClock = 162000000;
u32 __OSCoreClock = 486000000;
volatile OSHeapHandle __OSCurrHeap = 0;
OSErrorHandler OSErrorTable[15];

#define TIMER_CLOCK (162000000u / 4) // OS_TIMER_CLOCK, 40.5 MHz

static void* s_arena_base;
static void* s_arena_lo;
static void* s_arena_hi;
static size_t s_arena_size = 64u * 1024u * 1024u;
static unsigned s_mem_mb = 24;

static BOOL s_int_enabled = TRUE;
static u32 s_sound_mode = OS_SOUND_MODE_STEREO;
static u32 s_video_mode = OS_VIDEO_MODE_NTSC;
static u32 s_progressive = 0;
static unsigned char s_language = 0; // English
static unsigned long s_reset_code;
static OSResetCallback s_reset_cb;
static OSResetFunctionInfo* s_reset_funcs;

static OSContext* s_current_ctx;
static OSThread s_main_thread;
static int s_main_thread_init;

#ifdef _WIN32
static DWORD s_tls_index = TLS_OUT_OF_INDEXES;
#else
static pthread_key_t s_tls_key;
static int s_tls_init;
#endif

// ------------------------------------------------------------ wall clock
static double wall_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER freq;
    LARGE_INTEGER now;
    if (!freq.QuadPart)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&now);
    return (double) now.QuadPart / (double) freq.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
#endif
}

static double s_tick_base;
static int s_tick_init;

static void tick_ensure(void)
{
    if (!s_tick_init) {
        s_tick_base = wall_seconds();
        s_tick_init = 1;
    }
}

// ------------------------------------------------------------ TLS current thread
static OSThread* tls_get(void)
{
#ifdef _WIN32
    if (s_tls_index == TLS_OUT_OF_INDEXES)
        return &s_main_thread;
    return (OSThread*) TlsGetValue(s_tls_index);
#else
    if (!s_tls_init)
        return &s_main_thread;
    {
        void* v = pthread_getspecific(s_tls_key);
        return v ? (OSThread*) v : &s_main_thread;
    }
#endif
}

static void tls_set(OSThread* t)
{
#ifdef _WIN32
    if (s_tls_index == TLS_OUT_OF_INDEXES)
        s_tls_index = TlsAlloc();
    if (s_tls_index != TLS_OUT_OF_INDEXES)
        TlsSetValue(s_tls_index, t);
#else
    if (!s_tls_init) {
        pthread_key_create(&s_tls_key, NULL);
        s_tls_init = 1;
    }
    pthread_setspecific(s_tls_key, t);
#endif
}

// ------------------------------------------------------------ sdk2 config
static char s_disc_root[1024];
static char s_card_path[2][1024];
static char s_user_dir[1024];

void sdk2_set_disc_root(const char* path)
{
    if (path) {
        strncpy(s_disc_root, path, sizeof(s_disc_root) - 1);
        s_disc_root[sizeof(s_disc_root) - 1] = '\0';
    }
}

const char* sdk2_disc_root(void)
{
    return s_disc_root[0] ? s_disc_root : ".";
}

void sdk2_set_card_path(int chan, const char* path)
{
    if (chan >= 0 && chan < 2 && path) {
        strncpy(s_card_path[chan], path, sizeof(s_card_path[0]) - 1);
        s_card_path[chan][sizeof(s_card_path[0]) - 1] = '\0';
    }
}

const char* sdk2_card_path(int chan)
{
    static char def[2][64];
    if (chan < 0 || chan > 1)
        chan = 0;
    if (s_card_path[chan][0])
        return s_card_path[chan];
    snprintf(def[chan], sizeof(def[chan]), "melee_card_%c.mci",
             (char) ('A' + chan));
    return def[chan];
}

void sdk2_set_user_dir(const char* path)
{
    if (path) {
        strncpy(s_user_dir, path, sizeof(s_user_dir) - 1);
        s_user_dir[sizeof(s_user_dir) - 1] = '\0';
    }
}

const char* sdk2_user_dir(void)
{
    return s_user_dir[0] ? s_user_dir : ".";
}

void sdk2_set_mem_size_mb(unsigned mb)
{
    s_mem_mb = (mb == 48) ? 48 : 24;
}

unsigned sdk2_mem_size_mb(void)
{
    return s_mem_mb;
}

// GC address registry (used by AX sample lookup + DVD buffers)
#define ADDR_N 256
static struct {
    u32 gc;
    void* host;
    u32 size;
    int used;
} s_addr[ADDR_N];

void sdk2_addr_register(u32 gc_addr, void* host, u32 size)
{
    int i;
    for (i = 0; i < ADDR_N; i++) {
        if (!s_addr[i].used) {
            s_addr[i].used = 1;
            s_addr[i].gc = gc_addr;
            s_addr[i].host = host;
            s_addr[i].size = size;
            return;
        }
    }
}

void* sdk2_addr_host(u32 gc_addr)
{
    int i;
    // ARAM range is resolved by sdk2_ar; anything else: registry lookup.
    extern void* sdk2_ar_host(u32 aram_addr);
    void* p = sdk2_ar_host(gc_addr);
    if (p)
        return p;
    for (i = 0; i < ADDR_N; i++) {
        if (s_addr[i].used && gc_addr >= s_addr[i].gc &&
            gc_addr - s_addr[i].gc < s_addr[i].size) {
            return (u8*) s_addr[i].host + (gc_addr - s_addr[i].gc);
        }
    }
    return NULL;
}

void sdk2_addr_unregister(u32 gc_addr)
{
    int i;
    for (i = 0; i < ADDR_N; i++) {
        if (s_addr[i].used && s_addr[i].gc == gc_addr)
            s_addr[i].used = 0;
    }
}

// ------------------------------------------------------------ basic OS
unsigned long OSGetConsoleType(void)
{
    return OS_CONSOLE_PC_EMULATOR;
}

void OSInit(void)
{
    tick_ensure();
    if (!s_arena_base) {
        // Arena must honor LOWMEM_FLOOR (MRAM must never alias the ARAM
        // window); a NULL here fails fast instead of corrupting later.
        s_arena_base = sdk2_low_alloc(s_arena_size);
        if (!s_arena_base) {
            fputs("[sdk2] FATAL: no floored arena available\n", stderr);
            abort();
        }
        s_arena_lo = s_arena_base;
        s_arena_hi = (u8*) s_arena_base + s_arena_size;
    }
    if (!s_main_thread_init) {
        memset(&s_main_thread, 0, sizeof(s_main_thread));
        s_main_thread.state = OS_THREAD_STATE_RUNNING;
        s_main_thread.priority = 8;
        s_main_thread_init = 1;
        tls_set(&s_main_thread);
    }
}

void* OSGetArenaHi(void)
{
    return s_arena_hi;
}

void* OSGetArenaLo(void)
{
    return s_arena_lo;
}

void OSSetArenaHi(void* p)
{
    s_arena_hi = p;
}

void OSSetArenaLo(void* p)
{
    s_arena_lo = p;
}

static void* arena_bump_lo(u32 size, u32 align)
{
    uintptr_t p = ((uintptr_t) s_arena_lo + align - 1) & ~((uintptr_t) align - 1);
    s_arena_lo = (void*) (p + size);
    return (void*) p;
}

static void* arena_bump_hi(u32 size, u32 align)
{
    uintptr_t p = ((uintptr_t) s_arena_hi - size) & ~((uintptr_t) align - 1);
    s_arena_hi = (void*) p;
    return (void*) p;
}

void* OSAllocFromArenaLo(u32 size, u32 align)
{
    if (align == 0)
        align = 4;
    return arena_bump_lo(size, align);
}

void* OSAllocFromArenaHi(u32 size, u32 align)
{
    if (align == 0)
        align = 4;
    return arena_bump_hi(size, align);
}

u32 OSGetPhysicalMemSize(void)
{
    return s_mem_mb * 1024u * 1024u;
}

u32 OSGetConsoleSimulatedMemSize(void)
{
    return s_mem_mb * 1024u * 1024u;
}

void __OSPSInit()
{
}

u32 __OSGetDIConfig(void)
{
    return 0;
}

// ------------------------------------------------------------ tick / time
OSTick OSGetTick(void)
{
    double dt;
    tick_ensure();
    dt = wall_seconds() - s_tick_base;
    return (OSTick) (dt * TIMER_CLOCK);
}

OSTime OSGetTime(void)
{
    // seconds since 2000-01-01 in timer ticks
    time_t now = time(NULL);
    s64 secs = (s64) now - 946684800LL;
    return (OSTime) secs * (OSTime) TIMER_CLOCK + (OSGetTick() % TIMER_CLOCK);
}

void OSTicksToCalendarTime(OSTime ticks, OSCalendarTime* td)
{
    time_t secs;
    struct tm* tmv;
    if (!td)
        return;
    secs = (time_t) (ticks / (OSTime) TIMER_CLOCK) + 946684800LL;
    tmv = gmtime(&secs);
    if (!tmv) {
        memset(td, 0, sizeof(*td));
        return;
    }
    td->sec = tmv->tm_sec;
    td->min = tmv->tm_min;
    td->hour = tmv->tm_hour;
    td->mday = tmv->tm_mday;
    td->mon = tmv->tm_mon;
    td->year = tmv->tm_year + 1900;
    td->wday = tmv->tm_wday;
    td->yday = tmv->tm_yday;
    {
        OSTime rem = ticks % (OSTime) TIMER_CLOCK;
        if (rem < 0)
            rem = 0;
        td->msec = (int) (rem / (TIMER_CLOCK / 1000));
        td->usec = (int) ((rem % (TIMER_CLOCK / 1000)) /
                          (TIMER_CLOCK / 1000000));
    }
}

OSTime OSCalendarTimeToTicks(OSCalendarTime* td)
{
    struct tm tmv;
    time_t secs;
    if (!td)
        return 0;
    memset(&tmv, 0, sizeof(tmv));
    tmv.tm_sec = td->sec;
    tmv.tm_min = td->min;
    tmv.tm_hour = td->hour;
    tmv.tm_mday = td->mday;
    tmv.tm_mon = td->mon;
    tmv.tm_year = td->year - 1900;
    tmv.tm_isdst = 0;
#ifdef _WIN32
    secs = _mkgmtime(&tmv);
#else
    secs = timegm(&tmv);
#endif
    if (secs == (time_t) -1)
        return 0;
    return ((OSTime) secs - 946684800LL) * (OSTime) TIMER_CLOCK +
           td->msec * (TIMER_CLOCK / 1000);
}

BOOL OSEnableInterrupts(void)
{
    BOOL prev = s_int_enabled;
    s_int_enabled = TRUE;
    return prev;
}

BOOL OSDisableInterrupts(void)
{
    BOOL prev = s_int_enabled;
    s_int_enabled = FALSE;
    return prev;
}

BOOL OSRestoreInterrupts(BOOL level)
{
    BOOL prev = s_int_enabled;
    s_int_enabled = level;
    return prev;
}

// ------------------------------------------------------------ report / panic
void OSReport(char* msg, ...)
{
    va_list ap;
    va_start(ap, msg);
    vprintf(msg, ap);
    va_end(ap);
    fflush(stdout);
}

void OSPanic(const char* file, int line, const char* msg, ...)
{
    va_list ap;
    fprintf(stderr, "\n*** OS PANIC %s:%d: ", file ? file : "?", line);
    va_start(ap, msg);
    vfprintf(stderr, msg ? msg : "", ap);
    va_end(ap);
    fprintf(stderr, "\n");
    fflush(stderr);
    abort();
}

// ------------------------------------------------------------ RTC / sound
u32 OSGetSoundMode(void)
{
    return s_sound_mode;
}

void OSSetSoundMode(u32 mode)
{
    s_sound_mode = mode;
}

u32 OSGetVideoMode(void)
{
    return s_video_mode;
}

void OSSetVideoMode(u32 mode)
{
    s_video_mode = mode;
}

unsigned char OSGetLanguage(void)
{
    return s_language;
}

void OSSetLanguage(unsigned char language)
{
    s_language = language;
}

u32 OSGetProgressiveMode(void)
{
    return s_progressive;
}

void OSSetProgressiveMode(u32 mode)
{
    s_progressive = mode;
}

u16 OSGetWirelessID(s32 chan)
{
    (void) chan;
    return 0;
}

// ------------------------------------------------------------ reset / reboot
void OSRegisterResetFunction(OSResetFunctionInfo* info)
{
    if (!info)
        return;
    info->next = s_reset_funcs;
    info->prev = NULL;
    if (s_reset_funcs)
        s_reset_funcs->prev = info;
    s_reset_funcs = info;
}

void OSUnregisterResetFunction(OSResetFunctionInfo* info)
{
    if (!info)
        return;
    if (info->prev)
        info->prev->next = info->next;
    else
        s_reset_funcs = info->next;
    if (info->next)
        info->next->prev = info->prev;
}

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu)
{
    OSResetFunctionInfo* info = s_reset_funcs;
    (void) forceMenu;
    s_reset_code = resetCode;
    while (info) {
        info->func(FALSE);
        info = info->next;
    }
    // Restart/shutdown both terminate the process on PC.
    exit(reset == OS_RESET_SHUTDOWN ? 0 : 0);
}

unsigned long OSGetResetCode()
{
    return s_reset_code;
}

OSResetCallback OSSetResetCallback(OSResetCallback callback)
{
    OSResetCallback prev = s_reset_cb;
    s_reset_cb = callback;
    return prev;
}

BOOL OSGetResetSwitchState()
{
    return FALSE;
}

BOOL OSGetResetButtonState(void)
{
    return FALSE;
}

// ------------------------------------------------------------ error
OSErrorHandler OSSetErrorHandler(OSError error, OSErrorHandler handler)
{
    OSErrorHandler prev = NULL;
    if (error < OS_ERROR_MAX) {
        prev = OSErrorTable[error];
        OSErrorTable[error] = handler;
    }
    return prev;
}

// ------------------------------------------------------------ context
u32 OSGetStackPointer(void)
{
    u32 dummy = 0;
    return (u32) (uintptr_t) &dummy;
}

void OSDumpContext(OSContext* context)
{
    if (context)
        OSReport("OSContext %p (PC port: no register state)\n",
                 (void*) context);
}

void OSLoadContext(OSContext* context)
{
    (void) context;
}

u32 OSSaveContext(OSContext* context)
{
    (void) context;
    return 0;
}

void OSClearContext(OSContext* context)
{
    if (context)
        memset(context, 0, sizeof(*context));
}

OSContext* OSGetCurrentContext(void)
{
    return s_current_ctx;
}

void OSSetCurrentContext(OSContext* context)
{
    s_current_ctx = context;
}

void OSLoadFPUContext(OSContext* fpuContext)
{
    (void) fpuContext;
}

void OSSaveFPUContext(OSContext* fpuContext)
{
    (void) fpuContext;
}

u32 OSSwitchStack(u32 newsp)
{
    (void) newsp;
    return 0;
}

int OSSwitchFiber(u32 pc, u32 newsp)
{
    (void) pc;
    (void) newsp;
    return 0;
}

void OSInitContext(OSContext* context, u32 pc, u32 newsp)
{
    if (!context)
        return;
    memset(context, 0, sizeof(*context));
    context->srr0 = pc;
    context->gpr[1] = newsp;
}

void OSFillFPUContext(OSContext* context)
{
    (void) context;
}

// ------------------------------------------------------------ heap
#define HEAP_MAX 8
// The header is padded to 32 bytes so every payload is 32-byte aligned
// (retail DMA/ARAM paths assert src/dest/size % 32 == 0; the arena base
// itself is page-aligned, sizes are 32-rounded, so alignment holds).
typedef struct HeapBlock {
    size_t size;
    int used;
    struct HeapBlock* next;
    u8 _pad[20];
} HeapBlock;
_Static_assert(sizeof(HeapBlock) == 32, "HeapBlock must stay 32 bytes");

typedef struct {
    int used;
    HeapBlock* head;
} Heap;

static Heap s_heaps[HEAP_MAX];

void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps)
{
    (void) maxHeaps;
    memset(s_heaps, 0, sizeof(s_heaps));
    // heap 0 spans the given arena region
    s_heaps[0].used = 1;
    s_heaps[0].head = (HeapBlock*) arenaStart;
    s_heaps[0].head->size =
        (size_t) ((u8*) arenaEnd - (u8*) arenaStart) - sizeof(HeapBlock);
    s_heaps[0].head->used = 0;
    s_heaps[0].head->next = NULL;
    __OSCurrHeap = 0;
    // NOTE: returns the UNCHANGED arena start. Returning arenaEnd here
    // would make the caller (HSD_OSInit) move the arena lo past hi,
    // collapsing the arena so every later heap lives outside it.
    return arenaStart;
}

int OSCreateHeap(void* start, void* end)
{
    int i;
    for (i = 0; i < HEAP_MAX; i++) {
        if (!s_heaps[i].used) {
            s_heaps[i].used = 1;
            s_heaps[i].head = (HeapBlock*) start;
            s_heaps[i].head->size =
                (size_t) ((u8*) end - (u8*) start) - sizeof(HeapBlock);
            s_heaps[i].head->used = 0;
            s_heaps[i].head->next = NULL;
            return i;
        }
    }
    return -1;
}

void OSDestroyHeap(int heap)
{
    if (heap >= 0 && heap < HEAP_MAX)
        s_heaps[heap].used = 0;
}

void OSAddToHeap(int heap, void* start, void* end)
{
    HeapBlock* b;
    if (heap < 0 || heap >= HEAP_MAX || !s_heaps[heap].used)
        return;
    b = (HeapBlock*) start;
    b->size = (size_t) ((u8*) end - (u8*) start) - sizeof(HeapBlock);
    b->used = 0;
    b->next = s_heaps[heap].head;
    s_heaps[heap].head = b;
}

void* OSAllocFromHeap(int heap, unsigned long size)
{
    HeapBlock* b;
    size_t need;
    if (heap < 0 || heap >= HEAP_MAX || !s_heaps[heap].used)
        return malloc(size ? size : 1);
    need = (size + 31) & ~(size_t) 31;
    for (b = s_heaps[heap].head; b; b = b->next) {
        if (!b->used && b->size >= need) {
            if (b->size >= need + sizeof(HeapBlock) + 32) {
                HeapBlock* nb =
                    (HeapBlock*) ((u8*) (b + 1) + need);
                nb->size = b->size - need - sizeof(HeapBlock);
                nb->used = 0;
                nb->next = b->next;
                b->size = need;
                b->next = nb;
            }
            b->used = 1;
            return (void*) (b + 1);
        }
    }
    return NULL;
}

void OSFreeToHeap(int heap, void* ptr)
{
    HeapBlock* b;
    if (!ptr)
        return;
    if (heap < 0 || heap >= HEAP_MAX || !s_heaps[heap].used) {
        free(ptr);
        return;
    }
    b = ((HeapBlock*) ptr) - 1;
    b->used = 0;
    // coalesce with next
    while (b->next && !b->next->used &&
           (u8*) (b + 1) + b->size == (u8*) b->next) {
        b->size += sizeof(HeapBlock) + b->next->size;
        b->next = b->next->next;
    }
}

int OSSetCurrentHeap(int heap)
{
    int prev = __OSCurrHeap;
    __OSCurrHeap = heap;
    return prev;
}

long OSCheckHeap(int heap)
{
    HeapBlock* b;
    long free_bytes = 0;
    if (heap < 0 || heap >= HEAP_MAX || !s_heaps[heap].used)
        return -1;
    for (b = s_heaps[heap].head; b; b = b->next) {
        if (!b->used)
            free_bytes += (long) b->size;
    }
    return free_bytes;
}

unsigned long OSReferentSize(void* ptr)
{
    HeapBlock* b;
    if (!ptr)
        return 0;
    b = ((HeapBlock*) ptr) - 1;
    return (unsigned long) b->size;
}

void OSDumpHeap(int heap)
{
    OSReport("heap %d free %ld\n", heap, OSCheckHeap(heap));
}

void OSVisitAllocated(void (*visitor)(void*, unsigned long))
{
    int i;
    HeapBlock* b;
    if (!visitor)
        return;
    for (i = 0; i < HEAP_MAX; i++) {
        if (!s_heaps[i].used)
            continue;
        for (b = s_heaps[i].head; b; b = b->next) {
            if (b->used)
                visitor((void*) (b + 1), (unsigned long) b->size);
        }
    }
}

void* OSAllocFixed(void* rstart, void* rend)
{
    (void) rend;
    return rstart;
}

// ------------------------------------------------------------ cache (no-op on PC)
void DCInvalidateRange(void* addr, u32 nBytes)
{
    (void) addr;
    (void) nBytes;
}

void DCFlushRange(void* addr, u32 nBytes)
{
    (void) addr;
    (void) nBytes;
}

void DCStoreRange(void* addr, u32 nBytes)
{
    (void) addr;
    (void) nBytes;
}

void DCFlushRangeNoSync(void* addr, u32 nBytes)
{
    (void) addr;
    (void) nBytes;
}

void DCStoreRangeNoSync(void* addr, u32 nBytes)
{
    (void) addr;
    (void) nBytes;
}

void DCZeroRange(void* addr, u32 nBytes)
{
    if (addr)
        memset(addr, 0, nBytes);
}

void DCTouchRange(void* addr, u32 nBytes)
{
    (void) addr;
    (void) nBytes;
}

void ICInvalidateRange(void* addr, u32 nBytes)
{
    (void) addr;
    (void) nBytes;
}

void LCEnable(void)
{
}

void LCDisable(void)
{
}

void LCLoadBlocks(void* destTag, void* srcAddr, u32 numBlocks)
{
    (void) destTag;
    (void) srcAddr;
    (void) numBlocks;
}

void LCStoreBlocks(void* destAddr, void* srcTag, u32 numBlocks)
{
    (void) destAddr;
    (void) srcTag;
    (void) numBlocks;
}

u32 LCLoadData(void* destAddr, void* srcAddr, u32 nBytes)
{
    if (destAddr && srcAddr)
        memcpy(destAddr, srcAddr, nBytes);
    return 0;
}

u32 LCStoreData(void* destAddr, void* srcAddr, u32 nBytes)
{
    if (destAddr && srcAddr)
        memcpy(destAddr, srcAddr, nBytes);
    return 0;
}

u32 LCQueueLength(void)
{
    return 0;
}

void LCQueueWait(u32 len)
{
    (void) len;
}

void LCFlushQueue(void)
{
}

void __OSCacheInit(void)
{
}

void DCFlashInvalidate(void)
{
}

void DCEnable(void)
{
}

void DCDisable(void)
{
}

void DCFreeze(void)
{
}

void DCUnfreeze(void)
{
}

void DCTouchLoad(void* addr)
{
    (void) addr;
}

void DCBlockZero(void* addr)
{
    (void) addr;
}

void DCBlockStore(void* addr)
{
    (void) addr;
}

void DCBlockFlush(void* addr)
{
    (void) addr;
}

void DCBlockInvalidate(void* addr)
{
    (void) addr;
}

// ------------------------------------------------------------ alarms
static OSAlarm* s_alarm_list;
static int s_alarm_init;
#ifdef _WIN32
static CRITICAL_SECTION s_alarm_cs;
static int s_alarm_cs_init;
#else
static pthread_mutex_t s_alarm_mutex = PTHREAD_MUTEX_INITIALIZER;
#endif

static void alarm_lock(void)
{
#ifdef _WIN32
    if (!s_alarm_cs_init) {
        InitializeCriticalSection(&s_alarm_cs);
        s_alarm_cs_init = 1;
    }
    EnterCriticalSection(&s_alarm_cs);
#else
    pthread_mutex_lock(&s_alarm_mutex);
#endif
}

static void alarm_unlock(void)
{
#ifdef _WIN32
    LeaveCriticalSection(&s_alarm_cs);
#else
    pthread_mutex_unlock(&s_alarm_mutex);
#endif
}

#ifdef _WIN32
static DWORD WINAPI alarm_thread(LPVOID p)
{
    (void) p;
    for (;;) {
        OSAlarm* a;
        OSTime now;
        Sleep(1);
        alarm_lock();
        now = OSGetTick();
        for (a = s_alarm_list; a; a = a->next) {
            if ((s64) (now - a->fire) >= 0 && a->handler) {
                OSAlarmHandler h = a->handler;
                if (a->period > 0) {
                    a->fire += a->period;
                } else {
                    // one-shot: unlink
                    OSAlarm** pp = &s_alarm_list;
                    while (*pp && *pp != a)
                        pp = &(*pp)->next;
                    if (*pp)
                        *pp = a->next;
                    a->handler = NULL;
                }
                alarm_unlock();
                h(a, NULL);
                alarm_lock();
                break;
            }
        }
        alarm_unlock();
    }
    return 0;
}
#else
static void* alarm_thread(void* p)
{
    (void) p;
    for (;;) {
        OSAlarm* a;
        OSTime now;
        struct timespec ts = { 0, 1000000 };
        nanosleep(&ts, NULL);
        alarm_lock();
        now = OSGetTick();
        for (a = s_alarm_list; a; a = a->next) {
            if ((s64) (now - a->fire) >= 0 && a->handler) {
                OSAlarmHandler h = a->handler;
                if (a->period > 0) {
                    a->fire += a->period;
                } else {
                    OSAlarm** pp = &s_alarm_list;
                    while (*pp && *pp != a)
                        pp = &(*pp)->next;
                    if (*pp)
                        *pp = a->next;
                    a->handler = NULL;
                }
                alarm_unlock();
                h(a, NULL);
                alarm_lock();
                break;
            }
        }
        alarm_unlock();
    }
    return NULL;
}
#endif

static void alarm_thread_ensure(void)
{
    if (s_alarm_init)
        return;
    s_alarm_init = 1;
#ifdef _WIN32
    {
        HANDLE th =
            CreateThread(NULL, 0, alarm_thread, NULL, 0, NULL);
        if (th)
            CloseHandle(th);
    }
#else
    {
        pthread_t th;
        pthread_create(&th, NULL, alarm_thread, NULL);
        pthread_detach(th);
    }
#endif
}

BOOL OSCheckAlarmQueue(void)
{
    return s_alarm_list ? TRUE : FALSE;
}

void OSInitAlarm(void)
{
    alarm_thread_ensure();
}

void OSCreateAlarm(OSAlarm* alarm)
{
    if (!alarm)
        return;
    memset(alarm, 0, sizeof(*alarm));
    alarm_thread_ensure();
}

static void alarm_insert(OSAlarm* alarm)
{
    OSAlarm** pp = &s_alarm_list;
    OSCancelAlarm(alarm);
    while (*pp && (s64) ((*pp)->fire - alarm->fire) < 0)
        pp = &(*pp)->next;
    alarm->next = *pp;
    alarm->prev = NULL;
    if (*pp)
        (*pp)->prev = alarm;
    *pp = alarm;
}

void OSSetAlarm(OSAlarm* alarm, OSTime tick, OSAlarmHandler handler)
{
    if (!alarm)
        return;
    alarm_lock();
    alarm->handler = handler;
    alarm->fire = OSGetTick() + tick;
    alarm->period = 0;
    alarm_insert(alarm);
    alarm_unlock();
}

void OSSetAbsAlarm(struct OSAlarm* alarm, long long time,
                   void (*handler)(struct OSAlarm*, struct OSContext*))
{
    if (!alarm)
        return;
    alarm_lock();
    alarm->handler = handler;
    alarm->fire = (OSTime) time;
    alarm->period = 0;
    alarm_insert(alarm);
    alarm_unlock();
}

void OSSetPeriodicAlarm(OSAlarm* alarm, OSTime start, OSTime period,
                        OSAlarmHandler handler)
{
    if (!alarm)
        return;
    alarm_lock();
    alarm->handler = handler;
    alarm->start = start;
    alarm->period = period;
    alarm->fire = start;
    alarm_insert(alarm);
    alarm_unlock();
}

void OSCancelAlarm(OSAlarm* alarm)
{
    OSAlarm** pp;
    if (!alarm)
        return;
    for (pp = &s_alarm_list; *pp; pp = &(*pp)->next) {
        if (*pp == alarm) {
            *pp = alarm->next;
            if (alarm->next)
                alarm->next->prev = alarm->prev;
            alarm->next = alarm->prev = NULL;
            alarm->handler = NULL;
            return;
        }
    }
}

// ------------------------------------------------------------ threads
typedef struct {
    OSThread* t;
    void* (*fn)(void*);
    void* arg;
} ThreadStart;

#ifdef _WIN32
static DWORD WINAPI thread_entry(LPVOID p)
{
    ThreadStart* s = (ThreadStart*) p;
    OSThread* t = s->t;
    void* (*fn)(void*) = s->fn;
    void* arg = s->arg;
    free(s);
    tls_set(t);
    t->state = OS_THREAD_STATE_RUNNING;
    fn(arg);
    t->state = OS_THREAD_STATE_MORIBUND;
    return 0;
}
#else
static void* thread_entry(void* p)
{
    ThreadStart* s = (ThreadStart*) p;
    OSThread* t = s->t;
    void* (*fn)(void*) = s->fn;
    void* arg = s->arg;
    void* r;
    free(s);
    tls_set(t);
    t->state = OS_THREAD_STATE_RUNNING;
    r = fn(arg);
    t->state = OS_THREAD_STATE_MORIBUND;
    return r;
}
#endif

#define THREADS_MAX 32
static OSThread* s_threads[THREADS_MAX];
static int s_thread_count;
static s32 s_scheduler_disable;

// start-table backing store: OSCreateThread records func/param here,
// OSResumeThread recovers them on first resume (OSThread has no room).
static struct {
    OSThread* t;
    void* (*fn)(void*);
    void* arg;
} s_startab[THREADS_MAX];

void* (*sdk2_thread_lookup(OSThread* t, void** arg))(void*)
{
    int i;
    for (i = 0; i < THREADS_MAX; i++) {
        if (s_startab[i].t == t) {
            *arg = s_startab[i].arg;
            return s_startab[i].fn;
        }
    }
    return NULL;
}

void OSInitThreadQueue(OSThreadQueue* queue)
{
    if (queue) {
        queue->head = queue->tail = NULL;
    }
}

int OSCreateThread(struct OSThread* thread, void* (*func)(void*),
                   void* param, void* stack, unsigned long stackSize,
                   long priority, unsigned short attr)
{
    if (!thread || !func)
        return 0;
    memset(thread, 0, sizeof(*thread));
    thread->priority = (OSPriority) priority;
    thread->base = (OSPriority) priority;
    thread->state = OS_THREAD_STATE_READY;
    thread->attr = attr;
    thread->val = param;
    thread->stackBase = (u8*) stack;
    thread->stackEnd = (u32*) ((u8*) stack + stackSize);
    {
        int i;
        for (i = 0; i < THREADS_MAX; i++) {
            if (s_startab[i].t == NULL || s_startab[i].t == thread)
                break;
        }
        if (i < THREADS_MAX) {
            s_startab[i].t = thread;
            s_startab[i].fn = func;
            s_startab[i].arg = param;
        }
    }
    if (s_thread_count < THREADS_MAX)
        s_threads[s_thread_count++] = thread;
    return 1;
}

// sleep/wake side state (OSThreadQueue has no room for native sync)
#define SLEEPQ_MAX 32
typedef struct {
    OSThreadQueue* q;
#ifdef _WIN32
    HANDLE event;
#else
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    int waiters;
#endif
    int used;
} SleepQ;
static SleepQ s_sleepq[SLEEPQ_MAX];

static SleepQ* sleepq_get(OSThreadQueue* q)
{
    int i, free_slot = -1;
    for (i = 0; i < SLEEPQ_MAX; i++) {
        if (s_sleepq[i].used && s_sleepq[i].q == q)
            return &s_sleepq[i];
        if (!s_sleepq[i].used && free_slot < 0)
            free_slot = i;
    }
    if (free_slot < 0)
        return NULL;
    memset(&s_sleepq[free_slot], 0, sizeof(SleepQ));
    s_sleepq[free_slot].used = 1;
    s_sleepq[free_slot].q = q;
#ifdef _WIN32
    s_sleepq[free_slot].event = CreateEvent(NULL, FALSE, FALSE, NULL);
#else
    pthread_mutex_init(&s_sleepq[free_slot].mutex, NULL);
    pthread_cond_init(&s_sleepq[free_slot].cond, NULL);
#endif
    return &s_sleepq[free_slot];
}

void OSSleepThread(OSThreadQueue* queue)
{
    SleepQ* sq;
    OSThread* self;
    if (!queue)
        return;
    self = tls_get();
    self->state = OS_THREAD_STATE_WAITING;
    self->queue = queue;
    // enqueue
    self->link.next = NULL;
    self->link.prev = queue->tail;
    if (queue->tail)
        queue->tail->link.next = self;
    else
        queue->head = self;
    queue->tail = self;
    sq = sleepq_get(queue);
    if (!sq)
        return;
#ifdef _WIN32
    WaitForSingleObject(sq->event, INFINITE);
#else
    pthread_mutex_lock(&sq->mutex);
    sq->waiters++;
    pthread_cond_wait(&sq->cond, &sq->mutex);
    sq->waiters--;
    pthread_mutex_unlock(&sq->mutex);
#endif
    self->state = OS_THREAD_STATE_RUNNING;
    self->queue = NULL;
}

void OSWakeupThread(OSThreadQueue* queue)
{
    SleepQ* sq;
    OSThread* t;
    if (!queue || !queue->head)
        return;
    t = queue->head;
    queue->head = t->link.next;
    if (queue->head)
        queue->head->link.prev = NULL;
    else
        queue->tail = NULL;
    t->link.next = t->link.prev = NULL;
    t->state = OS_THREAD_STATE_READY;
    sq = sleepq_get(queue);
    if (!sq)
        return;
#ifdef _WIN32
    SetEvent(sq->event);
#else
    pthread_mutex_lock(&sq->mutex);
    pthread_cond_signal(&sq->cond);
    pthread_mutex_unlock(&sq->mutex);
#endif
}

s32 OSSuspendThread(OSThread* thread)
{
    OSThread* t = thread ? thread : tls_get();
    return ++t->suspend;
}

s32 OSResumeThread(OSThread* thread)
{
    ThreadStart* s;
    if (!thread)
        return -1;
    if (thread->suspend > 0)
        thread->suspend--;
    if (thread->suspend > 0)
        return thread->suspend;
    if (thread->state == OS_THREAD_STATE_READY) {
        // first resume: spawn the host thread
        void* arg = NULL;
        void* (*fn)(void*) = sdk2_thread_lookup(thread, &arg);
        if (!fn)
            return -1;
        s = (ThreadStart*) malloc(sizeof(ThreadStart));
        s->t = thread;
        s->fn = fn;
        s->arg = arg;
        thread->state = OS_THREAD_STATE_RUNNING;
#ifdef _WIN32
        {
            HANDLE h = CreateThread(NULL, 0, thread_entry, s, 0, NULL);
            if (h)
                CloseHandle(h);
        }
#else
        {
            pthread_t th;
            pthread_create(&th, NULL, thread_entry, s);
            pthread_detach(th);
        }
#endif
    }
    return 0;
}

void OSCancelThread(OSThread* thread)
{
    if (thread)
        thread->state = OS_THREAD_STATE_MORIBUND;
}

OSThread* OSGetCurrentThread(void)
{
    return tls_get();
}

s32 OSEnableScheduler(void)
{
    return --s_scheduler_disable;
}

s32 OSDisableScheduler(void)
{
    return ++s_scheduler_disable;
}

long OSCheckActiveThreads(void)
{
    long n = 0;
    int i;
    for (i = 0; i < s_thread_count; i++) {
        if (s_threads[i]->state != OS_THREAD_STATE_MORIBUND)
            n++;
    }
    return n + 1; // + main thread
}

// ------------------------------------------------------------ mutex / cond
#define MUTEX_MAX 64
typedef struct {
    OSMutex* m;
#ifdef _WIN32
    CRITICAL_SECTION cs;
#else
    pthread_mutex_t pm;
#endif
    int used;
} MutexSlot;
static MutexSlot s_mutexes[MUTEX_MAX];

static MutexSlot* mutex_get(OSMutex* m, int create)
{
    int i, free_slot = -1;
    for (i = 0; i < MUTEX_MAX; i++) {
        if (s_mutexes[i].used && s_mutexes[i].m == m)
            return &s_mutexes[i];
        if (!s_mutexes[i].used && free_slot < 0)
            free_slot = i;
    }
    if (!create || free_slot < 0)
        return NULL;
    memset(&s_mutexes[free_slot], 0, sizeof(MutexSlot));
    s_mutexes[free_slot].used = 1;
    s_mutexes[free_slot].m = m;
#ifdef _WIN32
    InitializeCriticalSection(&s_mutexes[free_slot].cs);
#else
    {
        pthread_mutexattr_t a;
        pthread_mutexattr_init(&a);
        pthread_mutexattr_settype(&a, PTHREAD_MUTEX_RECURSIVE);
        pthread_mutex_init(&s_mutexes[free_slot].pm, &a);
        pthread_mutexattr_destroy(&a);
    }
#endif
    return &s_mutexes[free_slot];
}

void OSInitMutex(struct OSMutex* mutex)
{
    if (!mutex)
        return;
    memset(mutex, 0, sizeof(*mutex));
    mutex_get(mutex, 1);
}

void OSLockMutex(struct OSMutex* mutex)
{
    MutexSlot* s;
    if (!mutex)
        return;
    s = mutex_get(mutex, 1);
    if (!s)
        return;
#ifdef _WIN32
    EnterCriticalSection(&s->cs);
#else
    pthread_mutex_lock(&s->pm);
#endif
    mutex->thread = tls_get();
    mutex->count++;
}

void OSUnlockMutex(struct OSMutex* mutex)
{
    MutexSlot* s;
    if (!mutex)
        return;
    s = mutex_get(mutex, 0);
    if (mutex->count > 0)
        mutex->count--;
    if (mutex->count == 0)
        mutex->thread = NULL;
    if (!s)
        return;
#ifdef _WIN32
    LeaveCriticalSection(&s->cs);
#else
    pthread_mutex_unlock(&s->pm);
#endif
}

BOOL OSTryLockMutex(struct OSMutex* mutex)
{
    MutexSlot* s;
    int ok = 0;
    if (!mutex)
        return FALSE;
    s = mutex_get(mutex, 1);
    if (!s)
        return FALSE;
#ifdef _WIN32
    ok = TryEnterCriticalSection(&s->cs) ? 1 : 0;
#else
    ok = (pthread_mutex_trylock(&s->pm) == 0);
#endif
    if (ok) {
        mutex->thread = tls_get();
        mutex->count++;
        return TRUE;
    }
    return FALSE;
}

#define COND_MAX 32
typedef struct {
    struct OSCond* c;
#ifdef _WIN32
    CONDITION_VARIABLE cv;
#else
    pthread_cond_t pc;
#endif
    int used;
} CondSlot;
static CondSlot s_conds[COND_MAX];

static CondSlot* cond_get(struct OSCond* c, int create)
{
    int i, free_slot = -1;
    for (i = 0; i < COND_MAX; i++) {
        if (s_conds[i].used && s_conds[i].c == c)
            return &s_conds[i];
        if (!s_conds[i].used && free_slot < 0)
            free_slot = i;
    }
    if (!create || free_slot < 0)
        return NULL;
    memset(&s_conds[free_slot], 0, sizeof(CondSlot));
    s_conds[free_slot].used = 1;
    s_conds[free_slot].c = c;
#ifdef _WIN32
    InitializeConditionVariable(&s_conds[free_slot].cv);
#else
    pthread_cond_init(&s_conds[free_slot].pc, NULL);
#endif
    return &s_conds[free_slot];
}

void OSInitCond(struct OSCond* cond)
{
    if (!cond)
        return;
    memset(cond, 0, sizeof(*cond));
    cond_get(cond, 1);
}

void OSWaitCond(struct OSCond* cond, struct OSMutex* mutex)
{
    CondSlot* cs;
    MutexSlot* ms;
    if (!cond || !mutex)
        return;
    cs = cond_get(cond, 1);
    ms = mutex_get(mutex, 0);
    if (!cs || !ms)
        return;
#ifdef _WIN32
    mutex->count = 0;
    mutex->thread = NULL;
    SleepConditionVariableCS(&cs->cv, &ms->cs, INFINITE);
    mutex->thread = tls_get();
    mutex->count = 1;
#else
    mutex->count = 0;
    mutex->thread = NULL;
    pthread_cond_wait(&cs->pc, &ms->pm);
    mutex->thread = tls_get();
    mutex->count = 1;
#endif
}

void OSSignalCond(struct OSCond* cond)
{
    CondSlot* cs;
    if (!cond)
        return;
    cs = cond_get(cond, 0);
    if (!cs)
        return;
#ifdef _WIN32
    WakeConditionVariable(&cs->cv);
#else
    pthread_cond_signal(&cs->pc);
#endif
}

// ------------------------------------------------------------ message queues
#define MQ_MAX 32
typedef struct {
    struct OSMessageQueue* q;
#ifdef _WIN32
    CRITICAL_SECTION cs;
    CONDITION_VARIABLE cv;
#else
    pthread_mutex_t mutex;
    pthread_cond_t cond;
#endif
    int used;
} MqSlot;
static MqSlot s_mqs[MQ_MAX];

static MqSlot* mq_get(struct OSMessageQueue* q, int create)
{
    int i, free_slot = -1;
    for (i = 0; i < MQ_MAX; i++) {
        if (s_mqs[i].used && s_mqs[i].q == q)
            return &s_mqs[i];
        if (!s_mqs[i].used && free_slot < 0)
            free_slot = i;
    }
    if (!create || free_slot < 0)
        return NULL;
    memset(&s_mqs[free_slot], 0, sizeof(MqSlot));
    s_mqs[free_slot].used = 1;
    s_mqs[free_slot].q = q;
#ifdef _WIN32
    InitializeCriticalSection(&s_mqs[free_slot].cs);
    InitializeConditionVariable(&s_mqs[free_slot].cv);
#else
    pthread_mutex_init(&s_mqs[free_slot].mutex, NULL);
    pthread_cond_init(&s_mqs[free_slot].cond, NULL);
#endif
    return &s_mqs[free_slot];
}

void OSInitMessageQueue(struct OSMessageQueue* mq, void* msgArray,
                        long msgCount)
{
    if (!mq)
        return;
    memset(mq, 0, sizeof(*mq));
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
    mq_get(mq, 1);
}

// flags: bit0 set (1) = non-blocking; 0 = block. Matches libogc convention.
static void mq_lock(MqSlot* s)
{
#ifdef _WIN32
    EnterCriticalSection(&s->cs);
#else
    pthread_mutex_lock(&s->mutex);
#endif
}

static void mq_unlock(MqSlot* s)
{
#ifdef _WIN32
    LeaveCriticalSection(&s->cs);
#else
    pthread_mutex_unlock(&s->mutex);
#endif
}

int OSSendMessage(struct OSMessageQueue* mq, void* msg, long flags)
{
    MqSlot* s;
    void** arr;
    if (!mq)
        return 0;
    s = mq_get(mq, 1);
    if (!s)
        return 0;
    arr = (void**) mq->msgArray;
    mq_lock(s);
    while (mq->usedCount >= mq->msgCount) {
        if (flags & 1) {
            mq_unlock(s);
            return 0;
        }
#ifdef _WIN32
        // CS is held here; SleepConditionVariableCS releases it atomically.
        SleepConditionVariableCS(&s->cv, &s->cs, INFINITE);
#else
        pthread_cond_wait(&s->cond, &s->mutex);
#endif
    }
    arr[(mq->firstIndex + mq->usedCount) % mq->msgCount] = msg;
    mq->usedCount++;
#ifdef _WIN32
    WakeConditionVariable(&s->cv);
#else
    pthread_cond_signal(&s->cond);
#endif
    mq_unlock(s);
    return 1;
}

int OSReceiveMessage(struct OSMessageQueue* mq, void* msg, long flags)
{
    MqSlot* s;
    void** arr;
    void** out = (void**) msg;
    if (!mq)
        return 0;
    s = mq_get(mq, 1);
    if (!s)
        return 0;
    arr = (void**) mq->msgArray;
    mq_lock(s);
    while (mq->usedCount == 0) {
        if (flags & 1) {
            mq_unlock(s);
            return 0;
        }
#ifdef _WIN32
        SleepConditionVariableCS(&s->cv, &s->cs, INFINITE);
#else
        pthread_cond_wait(&s->cond, &s->mutex);
#endif
    }
    if (out)
        *out = arr[mq->firstIndex];
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;
#ifdef _WIN32
    WakeConditionVariable(&s->cv);
#else
    pthread_cond_signal(&s->cond);
#endif
    mq_unlock(s);
    return 1;
}

int OSJamMessage(struct OSMessageQueue* mq, void* msg, long flags)
{
    MqSlot* s;
    void** arr;
    if (!mq)
        return 0;
    s = mq_get(mq, 1);
    if (!s)
        return 0;
    arr = (void**) mq->msgArray;
    mq_lock(s);
    while (mq->usedCount >= mq->msgCount) {
        if (flags & 1) {
            mq_unlock(s);
            return 0;
        }
#ifdef _WIN32
        SleepConditionVariableCS(&s->cv, &s->cs, INFINITE);
#else
        pthread_cond_wait(&s->cond, &s->mutex);
#endif
    }
    mq->firstIndex =
        (mq->firstIndex + mq->msgCount - 1) % mq->msgCount;
    arr[mq->firstIndex] = msg;
    mq->usedCount++;
#ifdef _WIN32
    WakeConditionVariable(&s->cv);
#else
    pthread_cond_signal(&s->cond);
#endif
    mq_unlock(s);
    return 1;
}

// ------------------------------------------------------------ PPC/debuglink
// The debug menu pokes the PPC MSR (FP/exception bits) and prints GC
// linker-script stack bounds. Neither exists on PC: MSR access is a
// no-op, and the stack symbols are inert dummies (debug output only).
u32 PPCMfmsr(void)
{
    return 0;
}

void PPCMtmsr(u32 newMSR)
{
    (void) newMSR;
}

unsigned char _stack_end[4] = { 0, 0, 0, 0 };
unsigned char _stack_addr[4] = { 0, 0, 0, 0 };
