#include "debug.h"

#include <stdio.h>

#include <dolphin/os.h>

struct DebugContext {
    OSContext context;
    u8 unk[0x10];
} HSD_Debug_804C2608;

static ReportCallback reportCallback;
static PanicCallback panicCallback;
#ifdef MELEE_PC_PORT
/// MWERKS libc exposes hookable stdout (`FILE.write_proc`, `__io_proc`,
/// `__file_handle`, `__idle_proc`); glibc does not. The report callback
/// path still works; plain stdout output goes through OSReport instead.
typedef void* __file_handle;
typedef void* __idle_proc;
typedef int (*__io_proc)(__file_handle, unsigned char*, size_t*, __idle_proc);
#endif
static __io_proc logFunc;

#ifdef MUST_MATCH
#pragma peephole off
#endif

static int report_func(__file_handle arg0, unsigned char* arg1, size_t* arg2,
                       __idle_proc arg3)
{
    if (reportCallback != NULL) {
        reportCallback(arg1, *arg2);
    }
    if (logFunc != NULL) {
        logFunc(arg0, arg1, arg2, arg3);
    }
    return 0;
}

void HSD_LogInit(void)
{
#ifdef MELEE_PC_PORT
    /* No hookable stdout on PC; OSReport routes output instead. */
    (void) report_func;
#else
    if (logFunc == NULL) {
        logFunc = stdout->write_proc;
    }
    stdout->write_proc = report_func;
    stdout->state.error = 0;
#endif
}

void __assert(const char* str, u32 arg1, const char* arg2)
{
    OSReport("assertion \"%s\" failed", arg2);
    HSD_Panic(str, arg1, "");
}

void HSD_Panic(const char* arg0, u32 line, const char* arg2)
{
    if (panicCallback != NULL) {
        OSSaveContext(&HSD_Debug_804C2608.context);
        OSReport("%s in %s on line %d.\n", arg2, arg0, line);
        panicCallback(&HSD_Debug_804C2608.context);
    }
    OSPanic(arg0, line, arg2);
}

void HSD_SetReportCallback(ReportCallback cb)
{
    reportCallback = cb;
}

void HSD_SetPanicCallback(PanicCallback cb)
{
    panicCallback = cb;
}
